// logdb.c - SQLite-backed request log.
//
// Threading: libmicrohttpd serves each connection on its own thread, so every
// logdb_* call is serialized through a single mutex. The connection itself is
// created once at startup and reused for the lifetime of the process; rows are
// appended via one prepared statement.
#include "logdb.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static sqlite3 *g_db = nullptr;
static sqlite3_stmt *g_insert = nullptr;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_path[CFG_STR_URL] = "";

static void now_iso8601(char *buf, size_t bufsz)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    gmtime_r(&tv.tv_sec, &tmv);
    strftime(buf, bufsz, "%Y-%m-%dT%H:%M:%S", &tmv);
    char frac[32];
    snprintf(frac, sizeof frac, ".%06ldZ", (long)tv.tv_usec);
    strncat(buf, frac, bufsz - strlen(buf) - 1u);
}

// One-line summary, e.g.:
//   [2026-08-17T20:57:01.123456Z] openai  /v1/chat/completions gpt-4o
//     -> deepseek/deepseek-v4-flash req=382B resp=214B status=200 dur=812ms
//   [2026-08-17T20:57:02.123456Z] ollama-chat /api/chat bhag
//     -> deepseek-vl/deepseek-v4.1-flash (image) req=12345B resp=~890B status=200 dur=812ms stream
static void print_summary(const RequestLog *rl)
{
    char ts[48];
    now_iso8601(ts, sizeof ts);
    char line[2048];
    int n = snprintf(line, sizeof line, "[%s] %s %s", ts, rl->protocol,
                     rl->endpoint);
    if (rl->requested_model[0] != '\0')
        n += snprintf(line + n, sizeof line - (size_t)n, " model=%s",
                      rl->requested_model);
    n += snprintf(line + n, sizeof line - (size_t)n, " -> %s/%s",
                  rl->routed_provider[0] != '\0' ? rl->routed_provider : "-",
                  rl->routed_model[0] != '\0' ? rl->routed_model : "-");
    if (rl->has_image)
        n += snprintf(line + n, sizeof line - (size_t)n, " (image)");
    if (rl->has_audio)
        n += snprintf(line + n, sizeof line - (size_t)n, " (audio)");
    if (rl->has_file)
        n += snprintf(line + n, sizeof line - (size_t)n, " (file)");
    n += snprintf(line + n, sizeof line - (size_t)n, " req=%zuB", rl->request_size);
    if (rl->stream && rl->response_size == 0u)
        n += snprintf(line + n, sizeof line - (size_t)n, " resp=~stream");
    else
        n += snprintf(line + n, sizeof line - (size_t)n, " resp=%zuB",
                      rl->response_size);
    n += snprintf(line + n, sizeof line - (size_t)n, " status=%ld dur=%lldms",
                  rl->status, rl->duration_ms);
    if (rl->trimmed_bytes > 0u)
        n += snprintf(line + n, sizeof line - (size_t)n, " trim=%zuB",
                      rl->trimmed_bytes);
    if (rl->stream)
        n += snprintf(line + n, sizeof line - (size_t)n, " stream");
    if (rl->error != nullptr && rl->error[0] != '\0')
        n += snprintf(line + n, sizeof line - (size_t)n, " error=%s", rl->error);
    n += snprintf(line + n, sizeof line - (size_t)n, "\n");
    if (n > 0)
        fwrite(line, 1u, (size_t)n, stdout);
    fflush(stdout);
}

bool logdb_init(const char *path, char errbuf[CFG_ERRBUF_SIZE])
{
    // NULL / empty path: persistence is disabled, not defaulted. Nothing is
    // opened or created; stdout summaries keep working (see logdb_record).
    if (path == nullptr || path[0] == '\0')
        return true;

    pthread_mutex_lock(&g_mtx);
    const char *dbpath = path;
    const int rc = sqlite3_open(dbpath, &g_db);
    if (rc != SQLITE_OK) {
        set_err(errbuf, "sqlite3_open(%s): %s", dbpath,
                g_db != nullptr ? sqlite3_errmsg(g_db) : "unknown error");
        if (g_db != nullptr) {
            sqlite3_close(g_db);
            g_db = nullptr;
        }
        pthread_mutex_unlock(&g_mtx);
        return false;
    }
    sqlite3_busy_timeout(g_db, 5000);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS requests ("
        " id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts               TEXT NOT NULL,"
        " protocol         TEXT NOT NULL,"
        " endpoint         TEXT NOT NULL,"
        " requested_model  TEXT NOT NULL,"
        " routed_provider  TEXT NOT NULL,"
        " routed_model     TEXT NOT NULL,"
        " has_image        INTEGER NOT NULL,"
        " has_audio        INTEGER NOT NULL DEFAULT 0,"
        " has_file         INTEGER NOT NULL DEFAULT 0,"
        " stream           INTEGER NOT NULL,"
        " request_size     INTEGER NOT NULL,"
        " response_size    INTEGER NOT NULL,"
        " status           INTEGER NOT NULL,"
        " duration_ms      INTEGER NOT NULL,"
        " trimmed_bytes    INTEGER NOT NULL DEFAULT 0,"
        " error            TEXT,"
        " request_body     TEXT,"
        " response_body    TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_requests_ts ON requests(ts);"
        "CREATE INDEX IF NOT EXISTS idx_requests_provider"
        " ON requests(routed_provider, ts);";
    char *errmsg = nullptr;
    if (sqlite3_exec(g_db, schema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        set_err(errbuf, "sqlite: %s", errmsg != nullptr ? errmsg : "schema failed");
        sqlite3_free(errmsg);
        sqlite3_close(g_db);
        g_db = nullptr;
        pthread_mutex_unlock(&g_mtx);
        return false;
    }
    // Migration for databases created before trimmed_bytes existed: adding the
    // column fails with "duplicate column" on fresh DBs, which is fine.
    sqlite3_exec(g_db, "ALTER TABLE requests ADD COLUMN trimmed_bytes"
                       " INTEGER NOT NULL DEFAULT 0;", nullptr, nullptr,
                 nullptr);
    // Migration for databases created before the audio/file flags existed.
    sqlite3_exec(g_db, "ALTER TABLE requests ADD COLUMN has_audio"
                       " INTEGER NOT NULL DEFAULT 0;", nullptr, nullptr,
                 nullptr);
    sqlite3_exec(g_db, "ALTER TABLE requests ADD COLUMN has_file"
                       " INTEGER NOT NULL DEFAULT 0;", nullptr, nullptr,
                 nullptr);

    const char *sql =
        "INSERT INTO requests (ts, protocol, endpoint, requested_model,"
        " routed_provider, routed_model, has_image, has_audio, has_file,"
        " stream, request_size, response_size, status, duration_ms,"
        " trimmed_bytes, error, request_body, response_body)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &g_insert, nullptr) != SQLITE_OK) {
        set_err(errbuf, "sqlite prepare: %s", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = nullptr;
        g_insert = nullptr;
        pthread_mutex_unlock(&g_mtx);
        return false;
    }
    snprintf(g_path, sizeof g_path, "%s", dbpath);
    pthread_mutex_unlock(&g_mtx);
    return true;
}

void logdb_record(const RequestLog *rl)
{
    if (rl == nullptr)
        return;

    print_summary(rl);

    if (g_db == nullptr || g_insert == nullptr)
        return;

    pthread_mutex_lock(&g_mtx);
    if (g_db == nullptr || g_insert == nullptr) {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    sqlite3_reset(g_insert);
    sqlite3_clear_bindings(g_insert);
    char ts[48];
    now_iso8601(ts, sizeof ts);
    sqlite3_bind_text(g_insert, 1, ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 2, rl->protocol, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 3, rl->endpoint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 4, rl->requested_model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 5, rl->routed_provider, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 6, rl->routed_model, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(g_insert, 7, rl->has_image ? 1 : 0);
    sqlite3_bind_int(g_insert, 8, rl->has_audio ? 1 : 0);
    sqlite3_bind_int(g_insert, 9, rl->has_file ? 1 : 0);
    sqlite3_bind_int(g_insert, 10, rl->stream ? 1 : 0);
    sqlite3_bind_int64(g_insert, 11, (sqlite3_int64)rl->request_size);
    sqlite3_bind_int64(g_insert, 12, (sqlite3_int64)rl->response_size);
    sqlite3_bind_int(g_insert, 13, (int)rl->status);
    sqlite3_bind_int64(g_insert, 14, (sqlite3_int64)rl->duration_ms);
    sqlite3_bind_int64(g_insert, 15, (sqlite3_int64)rl->trimmed_bytes);
    if (rl->error != nullptr)
        sqlite3_bind_text(g_insert, 16, rl->error, -1, SQLITE_TRANSIENT);
    if (rl->request_body != nullptr)
        sqlite3_bind_text(g_insert, 17, rl->request_body, -1, SQLITE_TRANSIENT);
    if (rl->response_body != nullptr)
        sqlite3_bind_text(g_insert, 18, rl->response_body, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(g_insert) != SQLITE_DONE)
        fprintf(stderr, "funcroute: sqlite insert failed: %s\n",
                sqlite3_errmsg(g_db));
    pthread_mutex_unlock(&g_mtx);
}

void logdb_close(void)
{
    pthread_mutex_lock(&g_mtx);
    if (g_insert != nullptr) {
        sqlite3_finalize(g_insert);
        g_insert = nullptr;
    }
    if (g_db != nullptr) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
    pthread_mutex_unlock(&g_mtx);
    if (g_path[0] != '\0')
        printf("funcroute: request log written to %s\n", g_path);
}
