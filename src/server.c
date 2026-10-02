// server.c - HTTP gateway: routes /v1/chat/completions to DeepSeek or
// OpenRouter (multimodal) depending on whether the request contains
// an image_url content part. libmicrohttpd serves, libcurl forwards,
// jansson inspects/rewrites the JSON.
#include "config.h"
#include "ollama.h"
#include "provider.h"
#include "logdb.h"

#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REQ_BODY_INIT 4096u
#define REQ_BODY_MAX  (64u * 1024u * 1024u) // 64 MiB hard cap

static const Config *g_cfg = nullptr;
static volatile sig_atomic_t g_stop = 0;

// ---- tiny growable byte buffer (glue for the MHD upload callback) ----
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} ByteBuf;

static bool buf_append(ByteBuf *b, const char *p, size_t n)
{
    if (n == 0u)
        return true;
    if (b->len > REQ_BODY_MAX || n > REQ_BODY_MAX - b->len)
        return false; // too large
    if (b->len + n + 1u > b->cap) {
        size_t ncap = b->cap == 0u ? REQ_BODY_INIT : b->cap;
        while (ncap < b->len + n + 1u)
            ncap *= 2u;
        char *nd = realloc(b->data, ncap);
        if (nd == nullptr)
            return false;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static void buf_free(ByteBuf *b)
{
    free(b->data);
    *b = (ByteBuf){0};
}

// Monotonic wall clock in milliseconds (for request duration logging).
static long long wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// ---- response helpers ----
static enum MHD_Result respond(struct MHD_Connection *conn, unsigned status,
                               const char *ct, const void *data, size_t len)
{
    struct MHD_Response *r =
        MHD_create_response_from_buffer(len, (void *)data, MHD_RESPMEM_MUST_COPY);
    if (r == nullptr)
        return MHD_NO;
    MHD_add_response_header(r, "Content-Type", ct);
    // Permissive CORS so browser-based frontends can call the gateway.
    MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(r, "Access-Control-Allow-Methods",
                            "GET, POST, OPTIONS");
    MHD_add_response_header(r, "Access-Control-Allow-Headers",
                            "Content-Type, Authorization");
    const enum MHD_Result rc = MHD_queue_response(conn, status, r);
    MHD_destroy_response(r);
    return rc;
}

static enum MHD_Result respond_error(struct MHD_Connection *conn,
                                     unsigned status, const char *msg)
{
    char body[512];
    size_t n = 0u;
    body[n++] = '{';
    const char json_err[] = "\"error\":{\"message\":\"";
    memcpy(body + n, json_err, sizeof json_err - 1u);
    n += sizeof json_err - 1u;
    for (const char *s = msg; *s != '\0' && n + 2u < sizeof body; ++s) {
        if (*s == '"' || *s == '\\') {
            body[n++] = '\\';
            body[n++] = *s;
        } else if (*s == '\n') {
            body[n++] = '\\';
            body[n++] = 'n';
        } else {
            body[n++] = *s;
        }
    }
    memcpy(body + n, "\"}}", 3u);
    n += 3u;
    return respond(conn, status, "application/json", body, n);
}

// ---- OpenAI-compatible model list (OpenRouter-style) ----
// OpenRouter serves GET /v1/models where every entry carries its
// "context_length"; OpenAI-compatible harnesses read that to display the
// context-window meter. Mirrors /api/tags: the single exported model
// (routing.ollama_model) or one entry per provider.
static char *build_models_json(const Config *cfg)
{
    json_t *root = json_object();
    json_t *data = json_array();
    const long created = 1704067200L; // fixed epoch, stable across restarts

    if (cfg->routing.ollama_model[0] != '\0') {
        json_t *m = json_object();
        json_object_set_new(m, "id", json_string(cfg->routing.ollama_model));
        json_object_set_new(m, "object", json_string("model"));
        json_object_set_new(m, "created", json_integer(created));
        json_object_set_new(m, "owned_by", json_string("funcroute"));
        json_object_set_new(m, "name", json_string(cfg->routing.ollama_model));
        json_object_set_new(m, "context_length",
                            json_integer((json_int_t)cfg->routing.advertised_context_length));
        json_array_append_new(data, m);
    } else {
        for (size_t i = 0u; i < cfg->provider_count; ++i) {
            const Provider *p = &cfg->providers[i];
            json_t *m = json_object();
            json_object_set_new(m, "id", json_string(p->model));
            json_object_set_new(m, "object", json_string("model"));
            json_object_set_new(m, "created", json_integer(created));
            json_object_set_new(m, "owned_by", json_string(p->name));
            json_object_set_new(m, "name", json_string(p->model));
            json_object_set_new(m, "context_length",
                                json_integer((json_int_t)cfg->routing.advertised_context_length));
            json_array_append_new(data, m);
        }
    }
    json_object_set_new(root, "object", json_string("list"));
    json_object_set_new(root, "data", data);
    char *s = json_dumps(root, 0);
    json_decref(root);
    return s;
}

// GET /v1/models/{id} -> single model entry (OpenRouter detail endpoint).
// Returns NULL if the id is not advertised.
static char *build_model_json(const Config *cfg, const char *id)
{
    json_t *root = json_object();
    json_t *data = json_array();
    const long created = 1704067200L;

    if (cfg->routing.ollama_model[0] != '\0') {
        if (strcmp(id, cfg->routing.ollama_model) != 0) {
            json_decref(root);
            return nullptr;
        }
        json_t *m = json_object();
        json_object_set_new(m, "id", json_string(cfg->routing.ollama_model));
        json_object_set_new(m, "object", json_string("model"));
        json_object_set_new(m, "created", json_integer(created));
        json_object_set_new(m, "owned_by", json_string("funcroute"));
        json_object_set_new(m, "name", json_string(cfg->routing.ollama_model));
        json_object_set_new(m, "context_length",
                            json_integer((json_int_t)cfg->routing.advertised_context_length));
        json_array_append_new(data, m);
    } else {
        bool found = false;
        for (size_t i = 0u; i < cfg->provider_count; ++i) {
            const Provider *p = &cfg->providers[i];
            if (strcmp(id, p->model) != 0)
                continue;
            found = true;
            json_t *m = json_object();
            json_object_set_new(m, "id", json_string(p->model));
            json_object_set_new(m, "object", json_string("model"));
            json_object_set_new(m, "created", json_integer(created));
            json_object_set_new(m, "owned_by", json_string(p->name));
            json_object_set_new(m, "name", json_string(p->model));
            json_object_set_new(m, "context_length",
                                json_integer((json_int_t)cfg->routing.advertised_context_length));
            json_array_append_new(data, m);
        }
        if (!found) {
            json_decref(root);
            return nullptr;
        }
    }
    json_object_set_new(root, "object", json_string("list"));
    json_object_set_new(root, "data", data);
    char *s = json_dumps(root, 0);
    json_decref(root);
    return s;
}

// ---- true SSE passthrough streaming ----
#define STREAM_BLOCK    (16u * 1024u)
#define STREAM_PIPE_MAX (1024u * 1024u) // backpressure high-water mark

// Producer thread (libcurl) -> bounded byte pipe -> MHD content reader.
// The upstream status/Content-Type are published before any body bytes so the
// handler can build the response with the real status; body bytes then flow
// to the client as they arrive instead of being buffered whole.
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    char  *buf;
    size_t len, cap;
    char  *resp;          // captured response body for the request log
    size_t resp_len, resp_cap;
    bool   header_done;   // upstream status line received
    bool   failed;        // transport failure before any headers
    bool   closed;        // producer finished writing
    bool   abort;         // client went away; stop producing
    long   status;        // upstream HTTP status
    char   content_type[128];
    char   err[CFG_ERRBUF_SIZE];
    _Atomic int refs;     // producer + handler/response holder
    long long streamed;   // body bytes produced
    long long t0_ms;
    const Provider *prov;
    const char *path;     // upstream endpoint
    char *openai_body;    // malloc'd rewritten request; freed via pipe_release
    char *request_copy;   // malloc'd original request body for logging
    RequestLog rl;        // prefilled by the handler
} StreamPipe;

static void pipe_release(StreamPipe *p)
{
    if (atomic_fetch_sub(&p->refs, 1) > 1)
        return;
    pthread_mutex_destroy(&p->mtx);
    pthread_cond_destroy(&p->cond);
    free(p->buf);
    free(p->resp);
    free(p->request_copy);
    free(p->openai_body);
    free(p);
}

// libcurl write callback: append bytes into the pipe, throttling at the
// high-water mark so a slow client applies backpressure on the upstream.
static size_t stream_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    StreamPipe *p = ud;
    const size_t n = size * nmemb;
    if (n == 0u)
        return 0u;
    pthread_mutex_lock(&p->mtx);
    while (p->len >= STREAM_PIPE_MAX && !p->abort)
        pthread_cond_wait(&p->cond, &p->mtx);
    if (p->abort) {
        pthread_mutex_unlock(&p->mtx);
        return 0u; // abort the transfer
    }
    if (p->len + n > p->cap) {
        size_t ncap = p->cap == 0u ? STREAM_BLOCK : p->cap;
        while (ncap < p->len + n)
            ncap *= 2u;
        char *nb = realloc(p->buf, ncap);
        if (nb == nullptr) {
            snprintf(p->err, sizeof p->err,
                     "stream buffer allocation failed");
            pthread_mutex_unlock(&p->mtx);
            return 0u;
        }
        p->buf = nb;
        p->cap = ncap;
    }
    memcpy(p->buf + p->len, ptr, n);
    p->len += n;
    p->streamed += (long long)n;

    // Capture the response body for the request log (bounded).
    if (p->resp_len < LOGDB_CAPTURE_MAX) {
        size_t want = n;
        if (want > LOGDB_CAPTURE_MAX - p->resp_len)
            want = LOGDB_CAPTURE_MAX - p->resp_len;
        if (p->resp_len + want + 1u > p->resp_cap) {
            size_t ncap = p->resp_cap == 0u ? STREAM_BLOCK : p->resp_cap;
            while (ncap < p->resp_len + want + 1u)
                ncap *= 2u;
            char *nr = realloc(p->resp, ncap);
            if (nr == nullptr) {
                free(p->resp);
                p->resp = nullptr;
                p->resp_len = p->resp_cap = 0u;
            } else {
                p->resp = nr;
                p->resp_cap = ncap;
            }
        }
        if (p->resp != nullptr) {
            memcpy(p->resp + p->resp_len, ptr, want);
            p->resp_len += want;
            p->resp[p->resp_len] = '\0';
        }
    }

    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
    return n;
}

// libcurl header callback: publish the upstream HTTP status and Content-Type
// so the handler can build the response with the real status code.
static size_t stream_header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    StreamPipe *p = ud;
    const size_t n = size * nmemb;
    if (n >= 5u && memcmp(ptr, "HTTP/", 5u) == 0) {
        const char *s = ptr + 5u;
        const char *end = ptr + n;
        while (s < end && *s != ' ' && *s != '\t')
            s++;
        while (s < end && (*s == ' ' || *s == '\t'))
            s++;
        if (s < end) {
            char *e = nullptr;
            const long code = strtol(s, &e, 10);
            if (e != s) {
                pthread_mutex_lock(&p->mtx);
                if (!p->header_done) {
                    p->status = code;
                    p->header_done = true;
                    pthread_cond_broadcast(&p->cond);
                }
                pthread_mutex_unlock(&p->mtx);
            }
        }
    } else {
        static const char prefix[] = "Content-Type:";
        if (n >= sizeof prefix - 1u &&
            strncasecmp(ptr, prefix, sizeof prefix - 1u) == 0) {
            const char *s = ptr + sizeof prefix - 1u;
            const char *end = ptr + n;
            while (s < end && (*s == ' ' || *s == '\t'))
                s++;
            size_t len = 0u;
            while (s + len < end && s[len] != '\r' && s[len] != '\n')
                len++;
            if (len > 0u && len < sizeof p->content_type) {
                pthread_mutex_lock(&p->mtx);
                memcpy(p->content_type, s, len);
                p->content_type[len] = '\0';
                pthread_mutex_unlock(&p->mtx);
            }
        }
    }
    return n;
}

// Producer: perform the upstream POST, feeding body bytes into the pipe.
static void *stream_thread(void *arg)
{
    StreamPipe *p = arg;
    char errbuf[CFG_ERRBUF_SIZE] = {0};

    char url[CFG_STR_URL + CFG_STR_ENDPOINT + 2u];
    snprintf(url, sizeof url, "%s%s", p->prov->base_url, p->path);
    char auth[CFG_STR_KEY + 32u];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", p->prov->api_key);

    CURL *curl = curl_easy_init();
    if (curl != nullptr) {
        struct curl_slist *hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, auth);
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
        hdrs = curl_slist_append(hdrs, "User-Agent: funcroute/0.1");
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, p->openai_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)strlen(p->openai_body));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, p);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, stream_header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, p);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, p->prov->timeout_secs);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        const CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) {
            pthread_mutex_lock(&p->mtx);
            const bool aborted = p->abort;
            pthread_mutex_unlock(&p->mtx);
            if (!aborted) // client abort is not an upstream error
                snprintf(errbuf, sizeof errbuf,
                         "upstream stream to %.160s failed: %s", url,
                         curl_easy_strerror(rc));
        }
    } else {
        snprintf(errbuf, sizeof errbuf, "curl_easy_init failed");
    }

    pthread_mutex_lock(&p->mtx);
    if (errbuf[0] != '\0')
        snprintf(p->err, sizeof p->err, "%s", errbuf);
    if (errbuf[0] != '\0' && !p->header_done)
        p->failed = true;
    p->closed = true;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);

    // Log: streamed byte count, real upstream status, duration, error.
    RequestLog rl = p->rl;
    rl.status = p->failed ? MHD_HTTP_BAD_GATEWAY : p->status;
    rl.response_size = (size_t)p->streamed;
    rl.duration_ms = wall_ms() - p->t0_ms;
    rl.error = p->err[0] != '\0' ? p->err : nullptr;
    rl.request_body = p->request_copy;
    rl.response_body = p->resp; // captured up to LOGDB_CAPTURE_MAX
    logdb_record(&rl);

    pipe_release(p);
    return nullptr;
}

// MHD content reader: hand pipe bytes to the client as they arrive.
static ssize_t stream_reader_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    StreamPipe *p = cls;
    pthread_mutex_lock(&p->mtx);
    while (p->len == 0u && !p->closed && !p->abort)
        pthread_cond_wait(&p->cond, &p->mtx);
    size_t n = p->len < max ? p->len : max;
    if (n > 0u) {
        memcpy(buf, p->buf, n);
        memmove(p->buf, p->buf + n, p->len - n);
        p->len -= n;
        pthread_cond_broadcast(&p->cond); // wake a blocked producer
    }
    pthread_mutex_unlock(&p->mtx);
    if (n > 0u)
        return (ssize_t)n;
    return MHD_CONTENT_READER_END_OF_STREAM;
}

// Response destroyed (stream finished or client aborted): stop the producer.
static void stream_free_cb(void *cls)
{
    StreamPipe *p = cls;
    pthread_mutex_lock(&p->mtx);
    p->abort = true;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
    pipe_release(p);
}

// Stream an OpenAI request upstream with true incremental delivery.
static enum MHD_Result serve_stream(struct MHD_Connection *conn,
                                    const Provider *prov, const char *endpoint,
                                    char *openai_body, const RequestLog *rl,
                                    long long t0)
{
    StreamPipe *p = calloc(1u, sizeof *p);
    if (p == nullptr) {
        free(openai_body);
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "out of memory");
    }
    pthread_mutex_init(&p->mtx, nullptr);
    pthread_cond_init(&p->cond, nullptr);
    atomic_init(&p->refs, 2); // producer + handler (transferred to response)
    p->prov = prov;
    p->path = endpoint;
    p->openai_body = openai_body;
    p->t0_ms = t0;
    p->rl = *rl;
    p->request_copy = malloc(rl->request_size + 1u);
    if (p->request_copy != nullptr) {
        memcpy(p->request_copy, rl->request_body, rl->request_size);
        p->request_copy[rl->request_size] = '\0';
    }

    pthread_t tid;
    if (pthread_create(&tid, nullptr, stream_thread, p) != 0) {
        p->abort = true;
        pipe_release(p); // producer ref
        pipe_release(p); // handler ref -> frees the pipe
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "failed to spawn stream thread");
    }
    pthread_detach(tid);

    // Wait for the upstream status (headers) or a transport failure.
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += prov->timeout_secs + 5;
    pthread_mutex_lock(&p->mtx);
    while (!p->header_done && !p->failed && !p->closed && !p->abort) {
        if (pthread_cond_timedwait(&p->cond, &p->mtx, &deadline) == ETIMEDOUT)
            break;
    }
    const bool ok = p->header_done;
    const long status = p->status;
    char ct[128];
    snprintf(ct, sizeof ct, "%s",
             p->content_type[0] != '\0' ? p->content_type
                                        : "text/event-stream");
    pthread_mutex_unlock(&p->mtx);

    if (!ok) {
        char err[CFG_ERRBUF_SIZE] = {0};
        pthread_mutex_lock(&p->mtx);
        p->abort = true;
        pthread_cond_broadcast(&p->cond);
        snprintf(err, sizeof err, "%s",
                 p->err[0] != '\0' ? p->err : "upstream did not respond");
        pthread_mutex_unlock(&p->mtx);
        pipe_release(p); // handler ref
        return respond_error(conn, MHD_HTTP_BAD_GATEWAY, err);
    }

    struct MHD_Response *resp = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, STREAM_BLOCK, &stream_reader_cb, p, &stream_free_cb);
    if (resp == nullptr) {
        pthread_mutex_lock(&p->mtx);
        p->abort = true;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->mtx);
        pipe_release(p); // handler ref
        return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "failed to create stream response");
    }
    MHD_add_response_header(resp, "Content-Type", ct);
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Access-Control-Allow-Methods",
                            "GET, POST, OPTIONS");
    MHD_add_response_header(resp, "Access-Control-Allow-Headers",
                            "Content-Type, Authorization");
    const enum MHD_Result rc = MHD_queue_response(conn, (unsigned)status, resp);
    MHD_destroy_response(resp); // handler ref transfers to the free callback
    return rc;
}

// ---- routing decision: which attachment kinds does the request carry? ----
// Attachment parts (image/audio/file/PDF) mean the request must go to a
// provider that accepts multimodal input. Which provider depends on the kind,
// because upstream models differ: DeepSeek VL takes images and PDFs but no
// audio, so audio needs an omni/Gemini model. The part type strings are
// configurable (image_content_type, audio_content_type, file_content_type).
typedef struct {
    bool image;
    bool audio;
    bool file;
} MediaKinds;

static MediaKinds detect_media(json_t *messages, const Config *cfg)
{
    MediaKinds mk = {false, false, false};
    if (!json_is_array(messages))
        return mk;
    size_t i;
    json_t *msg;
    json_array_foreach(messages, i, msg)
    {
        json_t *content = json_object_get(msg, "content");
        if (!json_is_array(content))
            continue;
        size_t j;
        json_t *part;
        json_array_foreach(content, j, part)
        {
            json_t *t = json_object_get(part, "type");
            if (!json_is_string(t))
                continue;
            const char *typ = json_string_value(t);
            if (cfg->routing.image_content_type[0] != '\0' &&
                strcmp(typ, cfg->routing.image_content_type) == 0)
                mk.image = true;
            else if (cfg->routing.audio_content_type[0] != '\0' &&
                     strcmp(typ, cfg->routing.audio_content_type) == 0)
                mk.audio = true;
            else if (cfg->routing.file_content_type[0] != '\0' &&
                     strcmp(typ, cfg->routing.file_content_type) == 0)
                mk.file = true;
        }
    }
    return mk;
}

// Choose the upstream provider for a request from the attachment kinds it
// carries. Attachment routes are per kind; when a request mixes kinds the most
// restrictive one wins (audio > file > image), because the chosen provider
// must accept every attachment in the request — e.g. audio needs an omni model,
// which also happens to read images and PDFs. Text-only requests go to the
// default provider.
static const char *pick_provider(const Config *cfg, MediaKinds mk)
{
    if (mk.audio)
        return cfg->routing.audio_provider;
    if (mk.file)
        return cfg->routing.file_provider;
    if (mk.image)
        return cfg->routing.image_provider;
    return cfg->routing.default_provider;
}

// ---- MHD per-connection state (the request body) ----
static void request_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                              enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)conn;
    (void)toe;
    if (*con_cls != nullptr) {
        buf_free(*con_cls);
        free(*con_cls);
        *con_cls = nullptr;
    }
}

static enum MHD_Result handle_request(void *cls, struct MHD_Connection *conn,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls)
{
    (void)cls;
    (void)version;
    const Config *cfg = g_cfg;

    // First call for this connection: allocate the body collector and wait
    // for MHD's final call (with *upload_data_size == 0) once the request
    // has been fully received (works for GET as well as POST).
    if (*con_cls == nullptr) {
        ByteBuf *b = calloc(1u, sizeof *b);
        if (b == nullptr)
            return MHD_NO;
        *con_cls = b;
        return MHD_YES;
    }
    ByteBuf *body = *con_cls;

    // Still receiving the request body?
    if (*upload_data_size > 0u) {
        if (!buf_append(body, upload_data, *upload_data_size)) {
            *upload_data_size = 0u;
            return MHD_NO;
        }
        *upload_data_size = 0u;
        return MHD_YES;
    }

    // ---- body complete (or none): dispatch ----
    if (strcmp(method, "OPTIONS") == 0) // CORS preflight
        return respond(conn, MHD_HTTP_NO_CONTENT, "text/plain", "", 0u);

    // OpenAI-compatible model discovery (OpenRouter-style): GET /v1/models
    // and GET /v1/models/{id} advertise each model's context_length so
    // harnesses can show the context-window meter. Also served at /models
    // for clients whose base URL does not include the /v1 prefix.
    if (strcmp(method, "GET") == 0 &&
        (strcmp(url, "/v1/models") == 0 || strcmp(url, "/models") == 0)) {
        char *s = build_models_json(cfg);
        if (s == nullptr)
            return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 "out of memory");
        const enum MHD_Result rc = respond(conn, MHD_HTTP_OK,
                                           "application/json", s, strlen(s));
        free(s);
        return rc;
    }
    if (strcmp(method, "GET") == 0 &&
        (strncmp(url, "/v1/models/", 11u) == 0 ||
         strncmp(url, "/models/", 8u) == 0)) {
        const char *id = (url[1] == 'v') ? url + 11u : url + 8u;
        char *s = build_model_json(cfg, id);
        if (s == nullptr)
            return respond_error(conn, MHD_HTTP_NOT_FOUND,
                                 "unknown model");
        const enum MHD_Result rc = respond(conn, MHD_HTTP_OK,
                                           "application/json", s, strlen(s));
        free(s);
        return rc;
    }

    // Ollama-compatible layer: /api/version, /api/tags, /api/ps, /api/show,
    // /api/chat, /api/generate (so coding harnesses see an Ollama server).
    if (strncmp(url, "/api/", 5u) == 0) {
        if (ollama_handle_request(conn, cfg, url, method, body->data, body->len))
            return MHD_YES;
        return respond_error(conn, MHD_HTTP_NOT_FOUND,
                             "unknown /api endpoint");
    }

    if (strcmp(method, "POST") == 0 && strcmp(url, cfg->routing.endpoint) == 0) {
        const long long t0 = wall_ms();
        RequestLog rl = {0};
        snprintf(rl.protocol, sizeof rl.protocol, "%s", "openai");
        snprintf(rl.endpoint, sizeof rl.endpoint, "%s", url);
        rl.request_size = body->len;
        rl.request_body = body->data;

        json_error_t jerr;
        json_t *root = json_loadb(body->data, body->len, 0, &jerr);
        if (root == nullptr) {
            char msg[256];
            snprintf(msg, sizeof msg, "invalid JSON: %s (line %d)", jerr.text,
                     jerr.line);
            rl.status = MHD_HTTP_BAD_REQUEST;
            rl.duration_ms = wall_ms() - t0;
            rl.error = msg;
            logdb_record(&rl);
            return respond_error(conn, MHD_HTTP_BAD_REQUEST, msg);
        }

        const MediaKinds mk = detect_media(json_object_get(root, "messages"), cfg);
        rl.has_image = mk.image;
        rl.has_audio = mk.audio;
        rl.has_file = mk.file;
        json_t *req_model = json_object_get(root, "model");
        if (json_is_string(req_model))
            snprintf(rl.requested_model, sizeof rl.requested_model, "%s",
                     json_string_value(req_model));

        const char *provider_name = pick_provider(cfg, mk);
        const Provider *prov = config_find(cfg, provider_name);
        if (prov == nullptr || !prov->available) {
            // The route is configured but its key is not set. Answer 503 and
            // name the variable, rather than sending the attachment to a model
            // that was never going to accept it.
            char msg[CFG_ERRBUF_SIZE];
            const bool named = prov != nullptr && prov->api_key_env[0] != '\0';
            snprintf(msg, sizeof msg, "routing provider \"%s\" has no API key%s%s",
                     provider_name, named ? "; set " : "",
                     named ? prov->api_key_env : "");
            json_decref(root);
            rl.status = MHD_HTTP_SERVICE_UNAVAILABLE;
            rl.duration_ms = wall_ms() - t0;
            rl.error = "routing provider has no API key";
            logdb_record(&rl);
            return respond_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, msg);
        }
        snprintf(rl.routed_provider, sizeof rl.routed_provider, "%s", prov->name);
        snprintf(rl.routed_model, sizeof rl.routed_model, "%s", prov->model);

        // Context budget: drop the oldest messages that don't fit. Keeps the
        // system prompt first/byte-identical (DeepSeek prefix caching) and
        // never drops image-bearing messages or the current turn; image
        // payload bytes don't count against the budget, so switching to an
        // attachment provider with a large image/audio/PDF payload doesn't
        // wipe out history.
        rl.trimmed_bytes =
            router_trim_messages(json_object_get(root, "messages"), cfg);

        // Rewrite the model to the provider's configured model, then re-serialize.
        json_object_set_new(root, "model", json_string(prov->model));
        if (prov->reasoning[0] != '\0')
            json_object_set_new(root, "reasoning_effort",
                                json_string(prov->reasoning));
        const bool stream = json_is_true(json_object_get(root, "stream"));
        rl.stream = stream;
        char *out = json_dumps(root, 0);
        json_decref(root);
        if (out == nullptr) {
            rl.status = MHD_HTTP_INTERNAL_SERVER_ERROR;
            rl.duration_ms = wall_ms() - t0;
            rl.error = "request re-serialization failed";
            logdb_record(&rl);
            return respond_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 "request re-serialization failed");
        }

        if (stream) {
            // True streaming: forward SSE tokens to the client as they arrive.
            return serve_stream(conn, prov, cfg->routing.endpoint, out, &rl,
                                t0);
        }

        Upstream up;
        char errbuf[CFG_ERRBUF_SIZE] = {0};
        if (!provider_forward(prov, cfg->routing.endpoint, out, false, &up,
                              errbuf)) {
            free(out);
            rl.status = MHD_HTTP_BAD_GATEWAY;
            rl.duration_ms = wall_ms() - t0;
            rl.error = errbuf;
            logdb_record(&rl);
            return respond_error(conn, MHD_HTTP_BAD_GATEWAY, errbuf);
        }
        free(out);

        rl.status = up.status;
        rl.response_size = up.len;
        rl.response_body = up.body;
        rl.duration_ms = wall_ms() - t0;
        logdb_record(&rl);

        const char *ct = up.content_type[0] != '\0' ? up.content_type
                                                    : "application/json";
        const enum MHD_Result rc =
            respond(conn, (unsigned)up.status, ct, up.body, up.len);
        upstream_free(&up);
        return rc;
    }

    if (strcmp(method, "GET") == 0 && strcmp(url, "/healthz") == 0) {
        static const char ok[] = "{\"status\":\"ok\"}";
        return respond(conn, MHD_HTTP_OK, "application/json", ok, sizeof ok - 1u);
    }

    return respond_error(conn, MHD_HTTP_NOT_FOUND, "not found");
}

// ---- main ----
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    printf(
        "usage: %s [options] [config.json]\n"
        "OpenAI/Ollama-compatible request router.\n\n"
        "  --config PATH   config file (default: config.json or $FUNCROUTE_CONFIG)\n"
        "  --log PATH      SQLite request log file (overrides database.path)\n"
        "  --db PATH       alias for --log\n"
        "  --no-log        disable the SQLite request log; only one-line stdout\n"
        "                  summaries are printed and no database file is created\n"
        "  -h, --help      show this help\n\n"
        "The config file may also be given as a positional argument.\n"
        "Environment: FUNCROUTE_LOG=<path> and FUNCROUTE_NO_LOG=1 do the same\n"
        "as --log / --no-log (CLI flags win).\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *cfg_path = getenv("FUNCROUTE_CONFIG");
    if (cfg_path != nullptr && cfg_path[0] == '\0')
        cfg_path = nullptr;

    // Log override: 0 = use config, 1 = force path, 2 = force disabled.
    int log_mode = 0;
    const char *log_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(a, "--no-log") == 0) {
            log_mode = 2;
        } else if (strncmp(a, "--log=", 6u) == 0) {
            log_mode = 1;
            log_path = a + 6u;
        } else if (strcmp(a, "--log") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "funcroute: --log requires a path\n");
                return EXIT_FAILURE;
            }
            log_mode = 1;
            log_path = argv[++i];
        } else if (strncmp(a, "--db=", 5u) == 0) {
            log_mode = 1;
            log_path = a + 5u;
        } else if (strcmp(a, "--db") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "funcroute: --db requires a path\n");
                return EXIT_FAILURE;
            }
            log_mode = 1;
            log_path = argv[++i];
        } else if (strncmp(a, "--config=", 9u) == 0) {
            cfg_path = a + 9u;
        } else if (strcmp(a, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "funcroute: --config requires a path\n");
                return EXIT_FAILURE;
            }
            cfg_path = argv[++i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "funcroute: unknown option %s (try --help)\n", a);
            return EXIT_FAILURE;
        } else {
            cfg_path = a; // positional config path (backwards compatible)
        }
    }
    if (cfg_path == nullptr)
        cfg_path = "config.json";

    // Environment fallbacks for the log override (CLI flags already won).
    if (log_mode == 0) {
        const char *env_no = getenv("FUNCROUTE_NO_LOG");
        const char *env_log = getenv("FUNCROUTE_LOG");
        if (env_no != nullptr && env_no[0] != '\0' && strcmp(env_no, "0") != 0) {
            log_mode = 2;
        } else if (env_log != nullptr && env_log[0] != '\0') {
            log_mode = 1;
            log_path = env_log;
        }
    }

    Config cfg = {0};
    char errbuf[CFG_ERRBUF_SIZE] = {0};
    if (!config_load(&cfg, cfg_path, errbuf)) {
        fprintf(stderr, "funcroute: config error: %s\n", errbuf);
        return EXIT_FAILURE;
    }

    // Apply the log override on top of the config's database.path.
    if (log_mode == 2) {
        cfg.db_path[0] = '\0'; // disable SQLite persistence
    } else if (log_mode == 1 && log_path != nullptr && log_path[0] != '\0') {
        snprintf(cfg.db_path, sizeof cfg.db_path, "%s", log_path);
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        fprintf(stderr, "funcroute: curl_global_init failed\n");
        return EXIT_FAILURE;
    }
    g_cfg = &cfg;

    char logerr[CFG_ERRBUF_SIZE] = {0};
    if (!logdb_init(cfg.db_path, logerr))
        fprintf(stderr, "funcroute: warning: request log disabled: %s\n",
                logerr);
    else if (cfg.db_path[0] != '\0')
        printf("funcroute: request log database: %s\n", cfg.db_path);
    else
        printf("funcroute: request log disabled (stdout summaries only)\n");

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION, cfg.server.port, nullptr, nullptr,
        &handle_request, nullptr, MHD_OPTION_CONNECTION_LIMIT,
        (unsigned)cfg.server.max_connections,
        MHD_OPTION_LISTENING_ADDRESS_REUSE, 1,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, nullptr, MHD_OPTION_END);
    if (daemon == nullptr) {
        fprintf(stderr, "funcroute: failed to start HTTP server on port %u\n",
                (unsigned)cfg.server.port);
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    printf("funcroute %s: listening on %s:%u (endpoint %s)\n", FUNCROUTE_VERSION,
           cfg.server.host, (unsigned)cfg.server.port, cfg.routing.endpoint);
    const struct { const char *label; const char *name; } routes[] = {
        {"text", cfg.routing.default_provider},
        {"image", cfg.routing.image_provider},
        {"file", cfg.routing.file_provider},
        {"audio", cfg.routing.audio_provider},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        const Provider *p = config_find(&cfg, routes[i].name);
        if (p == nullptr || !p->available) {
            // Not offered, and it says which key would turn it on. The text
            // route cannot reach here: config_load refuses to start without it.
            if (p != nullptr && p->api_key_env[0] != '\0')
                printf("  %-5s -> disabled: provider \"%s\" has no API key"
                       " (set %s)\n", routes[i].label, routes[i].name,
                       p->api_key_env);
            else
                printf("  %-5s -> disabled: provider \"%s\" has no API key\n",
                       routes[i].label, routes[i].name);
        } else {
            printf("  %-5s -> provider \"%s\" model \"%s\"\n", routes[i].label,
                   p->name, p->model);
        }
    }
    printf("  attachment part types: image \"%s\", audio \"%s\", file \"%s\"\n",
           cfg.routing.image_content_type, cfg.routing.audio_content_type,
           cfg.routing.file_content_type);
    if (cfg.routing.ollama_model[0] != '\0')
        printf("  ollama /api/tags exports model \"%s\"\n",
               cfg.routing.ollama_model);
    fflush(stdout);

    while (!g_stop)
        sleep(1);

    MHD_stop_daemon(daemon);
    logdb_close();
    curl_global_cleanup();
    puts("funcroute: stopped");
    return EXIT_SUCCESS;
}
