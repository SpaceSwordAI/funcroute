// logdb.h - per-request logging + SQLite persistence for funcroute.
//
// Every routed request (OpenAI /v1/chat/completions, Ollama /api/chat and
// /api/generate) is (a) summarized as one line on stdout and (b) stored as a
// row in a SQLite database when one is configured. All entry points are
// thread-safe: libmicrohttpd runs each connection on its own thread, so
// logdb guards the single shared connection with a mutex.
#pragma once

#include "config.h"

#include <stdbool.h>
#include <stddef.h>

// Everything worth knowing about one routed request.
typedef struct {
    char protocol[24];            // "openai" | "ollama-chat" | "ollama-generate"
    char endpoint[CFG_STR_ENDPOINT + 8u];
    char requested_model[CFG_STR_MODEL]; // model the client asked for
    char routed_provider[CFG_STR_NAME];  // provider chosen by routing
    char routed_model[CFG_STR_MODEL];    // model forced onto the request
    bool has_image;               // request carried an image part
    bool has_audio;               // request carried an audio part
    bool has_file;                // request carried a file/PDF part
    bool stream;                  // request asked for streaming
    size_t request_size;          // bytes in the original request body
    size_t response_size;         // bytes in the reply body (0 if unknown)
    long status;                  // HTTP status sent to the client
    long long duration_ms;        // time spent proxying, milliseconds
    size_t trimmed_bytes;         // bytes dropped by the context budget (0 = none)
    const char *error;            // short error description or NULL
    const char *request_body;     // full request body or NULL to skip
    const char *response_body;    // full response body or NULL to skip
} RequestLog;

// Maximum response bytes retained for the database when the reply is
// streamed (non-streamed replies are captured in full). Keeps the log
// database from growing without bound on long generations; when a streamed
// response exceeds this, response_body stores the first LOGDB_CAPTURE_MAX
// bytes and response_size still records the true streamed total.
#define LOGDB_CAPTURE_MAX (64u * 1024u * 1024u)

// Open (or create) the SQLite database at path. A NULL or empty path disables
// persistence entirely: no database is opened, no file is created or touched,
// and only the one-line stdout summaries are printed. Returns true in that
// case (disabled is a valid configuration, not an error). Not fatal on
// failure: returns false and fills errbuf, but logging to stdout keeps working.
[[nodiscard]] bool logdb_init(const char *path, char errbuf[CFG_ERRBUF_SIZE]);

// Record one request: prints a one-line summary to stdout and inserts a row
// into the database if it is open. Safe to call from multiple threads.
void logdb_record(const RequestLog *rl);

// Close the database (finalize statement, close handle). No-op if not open.
void logdb_close(void);
