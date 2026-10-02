// ollama.c - Ollama-compatible endpoints over the funcroute routing core.
//
// Conversion summary:
//   Ollama /api/chat    -> OpenAI chat.completions (messages, images[] base64
//                          becomes image_url parts, options -> sampling params)
//   Ollama /api/generate-> OpenAI chat.completions (prompt + images[])
//   OpenAI SSE response -> Ollama NDJSON lines, streamed progressively via a
//                          producer thread + synchronized LineQueue consumed by
//                          an MHD content-reader callback.
#include "ollama.h"

#include "provider.h"
#include "logdb.h"

#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h>
#include <openssl/evp.h>
#include <pthread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define NDJSON_BLOCK (64u * 1024u)
#define STREAM_LINE  (64u * 1024u)

// ---------- context budget trimming ----------
// Estimate the serialized size of one message (OpenAI parts and/or Ollama
// images[]). *media_out receives the bytes belonging to attachment payloads
// (base64 image/audio/file data), which the byte budget excludes so large
// attachments don't cannibalize conversation history when the request switches
// to the multimodal provider. A message carrying any such payload is also
// never dropped by the trimmer.
static size_t msg_bytes(json_t *m, size_t *media_out)
{
    size_t sz = 0u, img = 0u;
    if (m == nullptr)
        goto out;
    json_t *content = json_object_get(m, "content");
    if (json_is_string(content)) {
        sz += json_string_length(content);
    } else if (json_is_array(content)) {
        size_t j;
        json_t *part;
        json_array_foreach(content, j, part)
        {
            json_t *t = json_object_get(part, "type");
            const char *typ = json_is_string(t) ? json_string_value(t) : "";
            if (strcmp(typ, "image_url") == 0) {
                json_t *iu = json_object_get(part, "image_url");
                json_t *u = json_is_object(iu) ? json_object_get(iu, "url")
                                               : nullptr;
                if (json_is_string(u)) {
                    const size_t l = json_string_length(u);
                    sz += l;
                    img += l;
                }
            } else if (strcmp(typ, "input_audio") == 0) {
                // OpenAI audio input: {"input_audio":{"data":<b64>,"format":..}}
                json_t *ia = json_object_get(part, "input_audio");
                json_t *d = json_is_object(ia) ? json_object_get(ia, "data")
                                               : nullptr;
                if (json_is_string(d)) {
                    const size_t l = json_string_length(d);
                    sz += l;
                    img += l;
                }
            } else if (strcmp(typ, "file") == 0) {
                // OpenRouter-style file/PDF: {"file":{"file_data":<data URL>}}
                json_t *fo = json_object_get(part, "file");
                json_t *d = json_is_object(fo)
                                ? json_object_get(fo, "file_data")
                                : nullptr;
                if (json_is_string(d)) {
                    const size_t l = json_string_length(d);
                    sz += l;
                    img += l;
                }
            } else if (strcmp(typ, "text") == 0) {
                json_t *txt = json_object_get(part, "text");
                if (json_is_string(txt))
                    sz += json_string_length(txt);
            } else {
                char *d = json_dumps(part, JSON_COMPACT);
                if (d != nullptr) {
                    sz += strlen(d);
                    free(d);
                }
            }
        }
    } else if (content != nullptr) {
        char *d = json_dumps(content, JSON_COMPACT);
        if (d != nullptr) {
            sz += strlen(d);
            free(d);
        }
    }
    // Ollama-style raw base64 images[].
    json_t *imgs = json_object_get(m, "images");
    if (json_is_array(imgs)) {
        size_t j;
        json_t *s;
        json_array_foreach(imgs, j, s)
        {
            if (json_is_string(s)) {
                const size_t l = json_string_length(s);
                sz += l;
                img += l;
            }
        }
    }
out:
    if (media_out != nullptr)
        *media_out = img;
    return sz;
}

// Apply the configured context budget to a messages array in place: drop the
// oldest messages until the request fits, keeping - in original order - every
// system message, the last message (the current turn, possibly the attachment
// request), any message carrying attachment payload (image/audio/file), and
// then as many of the newest remaining messages as fit. The system prompt
// stays first and byte-identical, so DeepSeek's automatic prefix caching keeps
// working for the surviving prefix (the deepseek route); when the request
// switches to an attachment provider, attachment bytes are excluded from
// the byte budget and attachment-bearing messages are never dropped.
// A tool exchange (an assistant message carrying tool_calls plus the run of
// tool result messages immediately following it) is kept or dropped as a
// unit: upstream APIs reject a message list that contains a 'tool' message
// without a preceding assistant 'tool_calls' message ("Messages with role
// 'tool' must be a response to a preceding message with 'tool_calls'"), so
// trimming one half of an exchange would break the request. A 'tool' message
// with no tool_calls predecessor at all is invalid in any request and is
// always dropped.
// Returns the number of bytes dropped (0 = no change).
size_t router_trim_messages(json_t *messages, const Config *cfg)
{
    if (!json_is_array(messages) || cfg == nullptr)
        return 0u;
    const size_t byte_budget = cfg->routing.max_request_bytes;
    const size_t msg_budget = cfg->routing.max_messages;
    if (byte_budget == 0u && msg_budget == 0u)
        return 0u;

    const size_t n = json_array_size(messages);
    if (n < 3u) // system + one middle + last at minimum
        return 0u;

    size_t *sizes = calloc(n, sizeof *sizes);
    size_t *imgs = calloc(n, sizeof *imgs);
    bool *keep = calloc(n, sizeof *keep);
    if (sizes == nullptr || imgs == nullptr || keep == nullptr) {
        free(sizes);
        free(imgs);
        free(keep);
        return 0u;
    }

    size_t total = 0u, img_total = 0u;
    for (size_t i = 0u; i < n; ++i) {
        sizes[i] = msg_bytes(json_array_get(messages, i), &imgs[i]);
        total += sizes[i];
        img_total += imgs[i];
    }

    // Byte budget counts text only: attachment payloads ride on top.
    const size_t eff_budget = byte_budget + img_total;

    // ---- tool-exchange grouping ----
    // exch[i] = index of the exchange's assistant tool_calls message, or n
    //           if i is not part of a tool exchange. unanchored[i] marks a
    //           'tool' message with no tool_calls predecessor in the request
    //           (invalid regardless of trimming -> always dropped).
    size_t *exch = malloc(n * sizeof *exch);
    bool *unanchored = calloc(n, sizeof *unanchored);
    if (exch == nullptr || unanchored == nullptr) {
        free(exch);
        free(unanchored);
        free(sizes);
        free(imgs);
        free(keep);
        return 0u;
    }
    {
        bool in_tool_run = false; // inside a run of tool responses
        size_t run_start = n;     // assistant index of the current run
        for (size_t i = 0u; i < n; ++i) {
            exch[i] = n;
            json_t *m = json_array_get(messages, i);
            json_t *role = json_object_get(m, "role");
            const char *r = json_is_string(role) ? json_string_value(role)
                                                 : "";
            if (strcmp(r, "tool") == 0) {
                if (in_tool_run)
                    exch[i] = run_start;
                else
                    unanchored[i] = true;
            } else if (strcmp(r, "assistant") == 0) {
                json_t *tc = json_object_get(m, "tool_calls");
                if (json_is_array(tc) && json_array_size(tc) > 0u) {
                    in_tool_run = true;
                    run_start = i;
                    exch[i] = i;
                } else {
                    in_tool_run = false;
                }
            } else {
                in_tool_run = false; // system/user ends any tool run
            }
        }
    }

    // Always keep: system messages, the last message, any attachment messages.
    // If a force-kept message belongs to a tool exchange, the whole exchange
    // is kept so the tool_calls <-> tool pairing survives. Unanchored tool
    // messages can never be valid and are never kept.
    size_t kept_bytes = 0u, kept_count = 0u;
    for (size_t i = 0u; i < n; ++i) {
        if (unanchored[i])
            continue;
        json_t *m = json_array_get(messages, i);
        json_t *role = json_object_get(m, "role");
        const bool is_system = json_is_string(role) &&
                               strcmp(json_string_value(role), "system") == 0;
        const bool is_last = i == n - 1u;
        const bool is_image = imgs[i] > 0u;
        if (!is_system && !is_last && !is_image)
            continue;
        if (exch[i] != n) {
            for (size_t j = exch[i]; j < n && exch[j] == exch[i]; ++j) {
                if (!keep[j]) {
                    keep[j] = true;
                    kept_bytes += sizes[j];
                    kept_count++;
                }
            }
        } else {
            keep[i] = true;
            kept_bytes += sizes[i];
            kept_count++;
        }
    }

    // Keep the newest remaining messages (whole tool exchanges at a time)
    // while both budgets hold.
    for (size_t i = n - 2u; i >= 1u; --i) { // n >= 3 so this is safe
        if (keep[i] || unanchored[i])
            continue;
        // Evaluate the whole exchange this message belongs to: its members
        // are contiguous and the newest member is visited first, so the
        // group decision is made once at that point.
        const size_t g = exch[i] != n ? exch[i] : i;
        size_t g_end = g;
        while (g_end + 1u < n && exch[g_end + 1u] == g)
            g_end++;
        size_t g_bytes = 0u, g_count = 0u;
        for (size_t j = g; j <= g_end; ++j) {
            if (!keep[j]) {
                g_bytes += sizes[j];
                g_count++;
            }
        }
        const bool ok_bytes = byte_budget == 0u ||
                              kept_bytes + g_bytes <= eff_budget;
        const bool ok_count = msg_budget == 0u ||
                              kept_count + g_count <= msg_budget;
        if (!ok_bytes || !ok_count) {
            // Stop here: this exchange and everything older is dropped.
            break;
        }
        for (size_t j = g; j <= g_end; ++j) {
            if (!keep[j]) {
                keep[j] = true;
                kept_bytes += sizes[j];
                kept_count++;
            }
        }
    }

    // Drop a kept assistant tool_calls message that has no kept tool response
    // after it (unless it is the last message). Trimming keeps exchanges
    // atomic, so this only fires on malformed input such as a lone
    // tool_calls message with no following tool results.
    {
        size_t *kept_idx = malloc(n * sizeof *kept_idx);
        if (kept_idx == nullptr) {
            free(exch);
            free(unanchored);
            free(sizes);
            free(imgs);
            free(keep);
            return 0u;
        }
        size_t nk = 0u;
        for (size_t i = 0u; i < n; ++i)
            if (keep[i])
                kept_idx[nk++] = i;
        for (size_t k = 0u; k + 1u < nk; ++k) {
            json_t *m = json_array_get(messages, kept_idx[k]);
            json_t *tc = json_object_get(m, "tool_calls");
            if (!json_is_array(tc) || json_array_size(tc) == 0u)
                continue;
            json_t *nx = json_array_get(messages, kept_idx[k + 1u]);
            json_t *nr = json_object_get(nx, "role");
            if (json_is_string(nr) &&
                strcmp(json_string_value(nr), "tool") == 0)
                continue;
            keep[kept_idx[k]] = false;
            kept_bytes -= sizes[kept_idx[k]];
            kept_count--;
        }
        free(kept_idx);
    }

    if (kept_count == n) { // nothing was dropped
        free(exch);
        free(unanchored);
        free(sizes);
        free(imgs);
        free(keep);
        return 0u;
    }

    json_t *na = json_array();
    if (na != nullptr) {
        for (size_t i = 0u; i < n; ++i) {
            if (keep[i])
                json_array_append(na, json_incref(json_array_get(messages, i)));
        }
        json_array_clear(messages);
        for (size_t i = 0u; i < json_array_size(na); ++i)
            json_array_append(messages, json_incref(json_array_get(na, i)));
        json_decref(na);
    }
    const size_t dropped = total - kept_bytes;
    free(exch);
    free(unanchored);
    free(sizes);
    free(imgs);
    free(keep);
    return dropped;
}

// ---------- small helpers ----------
static char *json_escape(const char *s)
{
    if (s == nullptr)
        s = "";
    const size_t cap = strlen(s) * 2u + 8u;
    char *out = malloc(cap);
    if (out == nullptr)
        return nullptr;
    size_t j = 0u;
    for (const char *p = s; *p != '\0' && j + 8u < cap; ++p) {
        switch (*p) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:
            if ((unsigned char)*p < 0x20u) {
                snprintf(out + j, cap - j, "\\u%04x", (unsigned)*p);
                j += 6u;
            } else {
                out[j++] = *p;
            }
        }
    }
    out[j] = '\0';
    return out;
}

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

static long long monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Best-effort MIME sniff from the first bytes of base64 image data.
static const char *sniff_image_mime(const char *b64, size_t len)
{
    if (b64 == nullptr || len < 32u)
        return "image/png";
    unsigned char dec[24] = {0};
    const int n = EVP_DecodeBlock(dec, (const unsigned char *)b64, 32);
    if (n < 4)
        return "image/png";
    if (dec[0] == 0x89 && dec[1] == 'P' && dec[2] == 'N' && dec[3] == 'G')
        return "image/png";
    if (dec[0] == 0xFF && dec[1] == 0xD8 && dec[2] == 0xFF)
        return "image/jpeg";
    if (dec[0] == 'G' && dec[1] == 'I' && dec[2] == 'F')
        return "image/gif";
    if (dec[0] == 'R' && dec[1] == 'I' && dec[2] == 'F' && dec[3] == 'F' &&
        n >= 12 && dec[8] == 'W' && dec[9] == 'E' && dec[10] == 'B' &&
        dec[11] == 'P')
        return "image/webp";
    return "image/png";
}

static void add_image_parts(json_t *parts, json_t *images)
{
    size_t i;
    json_t *img;
    json_array_foreach(images, i, img)
    {
        if (!json_is_string(img))
            continue;
        const char *b64 = json_string_value(img);
        json_t *part = json_object();
        json_object_set_new(part, "type", json_string("image_url"));
        json_t *iu = json_object();
        char url[64u + strlen(b64) + 1u];
        snprintf(url, sizeof url, "data:%s;base64,%s",
                 sniff_image_mime(b64, strlen(b64)), b64);
        json_object_set_new(iu, "url", json_string(url));
        json_object_set_new(part, "image_url", iu);
        json_array_append_new(parts, part);
    }
}

static void apply_options(json_t *root, const json_t *in)
{
    json_t *opts = json_object_get(in, "options");
    if (!json_is_object(opts))
        return;
    json_t *v;
    if ((v = json_object_get(opts, "temperature")) != nullptr) {
        if (json_is_real(v))
            json_object_set_new(root, "temperature", json_real(json_real_value(v)));
        else if (json_is_integer(v))
            json_object_set_new(root, "temperature",
                                json_real((double)json_integer_value(v)));
    }
    if ((v = json_object_get(opts, "top_p")) != nullptr && json_is_real(v))
        json_object_set_new(root, "top_p", json_real(json_real_value(v)));
    if ((v = json_object_get(opts, "num_predict")) != nullptr &&
        json_is_integer(v))
        json_object_set_new(root, "max_tokens",
                            json_integer(json_integer_value(v)));
    if ((v = json_object_get(opts, "seed")) != nullptr && json_is_integer(v))
        json_object_set_new(root, "seed", json_integer(json_integer_value(v)));
    if ((v = json_object_get(opts, "presence_penalty")) != nullptr &&
        json_is_real(v))
        json_object_set_new(root, "presence_penalty",
                            json_real(json_real_value(v)));
    if ((v = json_object_get(opts, "frequency_penalty")) != nullptr &&
        json_is_real(v))
        json_object_set_new(root, "frequency_penalty",
                            json_real(json_real_value(v)));
}

// ---------- Ollama -> OpenAI conversion ----------
// Returns a malloc'd OpenAI chat.completions body; *prov_out = routed provider;
// *req_model_out = the model the Ollama client asked for (echoed back);
// *has_image_out = whether the image (multimodal) route was taken; the image
//                  route uses routing.image_provider.
static char *convert_request(const Config *cfg, const json_t *in, bool stream,
                             bool generate, const Provider **prov_out,
                             bool *has_image_out, char *req_model_out,
                             size_t req_model_sz, char *errbuf)
{
    json_t *messages = json_array();

    json_t *sys = json_object_get(in, "system");
    if (json_is_string(sys)) {
        json_t *m = json_object();
        json_object_set_new(m, "role", json_string("system"));
        json_object_set_new(m, "content", json_string(json_string_value(sys)));
        json_array_append_new(messages, m);
    }

    bool has_image = false;
    if (generate) {
        json_t *m = json_object();
        json_object_set_new(m, "role", json_string("user"));
        json_t *prompt = json_object_get(in, "prompt");
        json_t *images = json_object_get(in, "images");
        const bool has_imgs =
            json_is_array(images) && json_array_size(images) > 0u;
        if (has_imgs) {
            has_image = true;
            json_t *parts = json_array();
            json_t *tp = json_object();
            json_object_set_new(tp, "type", json_string("text"));
            json_object_set_new(tp, "text",
                                json_string(json_is_string(prompt)
                                                ? json_string_value(prompt)
                                                : ""));
            json_array_append_new(parts, tp);
            add_image_parts(parts, images);
            json_object_set_new(m, "content", parts);
        } else {
            json_object_set_new(m, "content",
                                json_string(json_is_string(prompt)
                                                ? json_string_value(prompt)
                                                : ""));
        }
        json_array_append_new(messages, m);
    } else {
        json_t *ollama_msgs = json_object_get(in, "messages");
        if (!json_is_array(ollama_msgs)) {
            set_err(errbuf, "missing \"messages\" array");
            json_decref(messages);
            return nullptr;
        }
        size_t i;
        json_t *msg;
        json_array_foreach(ollama_msgs, i, msg)
        {
            json_t *m = json_object();
            json_t *role = json_object_get(msg, "role");
            if (json_is_string(role))
                json_object_set(m, "role", role);
            json_t *images = json_object_get(msg, "images");
            const bool has_imgs =
                json_is_array(images) && json_array_size(images) > 0u;
            if (has_imgs) {
                has_image = true;
                json_t *parts = json_array();
                json_t *content = json_object_get(msg, "content");
                if (json_is_string(content)) {
                    json_t *tp = json_object();
                    json_object_set_new(tp, "type", json_string("text"));
                    json_object_set_new(tp, "text",
                                        json_string(json_string_value(content)));
                    json_array_append_new(parts, tp);
                }
                add_image_parts(parts, images);
                json_object_set_new(m, "content", parts);
            } else {
                json_object_set(m, "content",
                                json_object_get(msg, "content"));
            }
            json_array_append_new(messages, m);
        }
    }

    // Ollama's API can only express images (the `images` array), so this route
    // never carries audio/file parts: image requests use image_provider.
    const Provider *prov = config_find(
        cfg, has_image ? cfg->routing.image_provider
                       : cfg->routing.default_provider);
    if (prov == nullptr || !prov->available) {
        const char *route = has_image ? cfg->routing.image_provider
                                      : cfg->routing.default_provider;
        const bool named = prov != nullptr && prov->api_key_env[0] != '\0';
        set_err(errbuf, "routing provider \"%s\" has no API key%s%s", route,
                named ? "; set " : "", named ? prov->api_key_env : "");
        json_decref(messages);
        return nullptr;
    }

    json_t *root = json_object();
    json_object_set_new(root, "model", json_string(prov->model));
    if (prov->reasoning[0] != '\0')
        json_object_set_new(root, "reasoning_effort",
                            json_string(prov->reasoning));
    json_object_set_new(root, "messages", messages);
    apply_options(root, in);
    json_object_set_new(root, "stream", json_boolean(stream));

    char *out = json_dumps(root, 0);
    json_decref(root);

    json_t *rm = json_object_get(in, "model");
    if (json_is_string(rm))
        snprintf(req_model_out, req_model_sz, "%s", json_string_value(rm));
    else
        snprintf(req_model_out, req_model_sz, "%s", prov->model);

    *prov_out = prov;
    if (has_image_out != nullptr)
        *has_image_out = has_image;
    return out;
}

// ---------- synchronized line queue (producer thread -> MHD reader) ----------
typedef struct LineNode {
    char *data;
    size_t len;
    struct LineNode *next;
} LineNode;

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    LineNode *head;
    LineNode *tail;
    _Atomic int refs;
    bool closed;
} LineQueue;

static void lq_init(LineQueue *q)
{
    pthread_mutex_init(&q->mtx, nullptr);
    pthread_cond_init(&q->cond, nullptr);
    q->head = nullptr;
    q->tail = nullptr;
    q->closed = false;
    atomic_init(&q->refs, 1);
}

static void lq_retain(LineQueue *q) { atomic_fetch_add(&q->refs, 1); }

static void lq_push(LineQueue *q, const char *data, size_t len)
{
    LineNode *n = malloc(sizeof *n);
    if (n == nullptr)
        return;
    n->data = malloc(len + 1u);
    if (n->data == nullptr) {
        free(n);
        return;
    }
    memcpy(n->data, data, len);
    n->data[len] = '\0';
    n->len = len;
    n->next = nullptr;
    pthread_mutex_lock(&q->mtx);
    if (q->tail != nullptr)
        q->tail->next = n;
    else
        q->head = n;
    q->tail = n;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

static void lq_close(LineQueue *q)
{
    pthread_mutex_lock(&q->mtx);
    q->closed = true;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

// true + malloc'd line (caller frees) if available; false when closed & empty
static bool lq_pop(LineQueue *q, char **out, size_t *outlen)
{
    pthread_mutex_lock(&q->mtx);
    while (q->head == nullptr && !q->closed)
        pthread_cond_wait(&q->cond, &q->mtx);
    LineNode *n = q->head;
    if (n == nullptr) {
        pthread_mutex_unlock(&q->mtx);
        return false;
    }
    q->head = n->next;
    if (q->head == nullptr)
        q->tail = nullptr;
    pthread_mutex_unlock(&q->mtx);
    *out = n->data;
    *outlen = n->len;
    free(n);
    return true;
}

static void lq_release(LineQueue *q)
{
    if (atomic_fetch_sub(&q->refs, 1) > 1)
        return; // other holder remains
    pthread_mutex_lock(&q->mtx);
    LineNode *n = q->head;
    while (n != nullptr) {
        LineNode *nx = n->next;
        free(n->data);
        free(n);
        n = nx;
    }
    pthread_mutex_unlock(&q->mtx);
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->cond);
    free(q);
}

// ---------- streaming: OpenAI SSE -> Ollama NDJSON ----------
typedef struct {
    const Config *cfg;
    const Provider *prov;
    const char *path;   // upstream endpoint (cfg->routing.endpoint)
    char orig_path[CFG_STR_ENDPOINT + 8u]; // original request path, e.g. /api/chat
    char *openai_body; // malloc'd; freed by the producer thread
    bool chat;         // /api/chat vs /api/generate response shape
    char req_model[128];
    char created_at[48];
    LineQueue *q;
    char line[STREAM_LINE];
    size_t line_len;
    long long t0_ns;
    long eval_count;
    bool done_pushed;
    // --- request logging (filled by handle_generation, used by stream_thread) ---
    char protocol[24];
    bool has_image;
    char *orig_body;    // malloc'd copy of the original request body
    size_t orig_len;
    long long streamed; // NDJSON bytes emitted to the client
    size_t trimmed_bytes; // context budget bytes dropped before conversion
    char error[CFG_ERRBUF_SIZE];
    char *resp;         // captured NDJSON response body for the request log
    size_t resp_len, resp_cap;
} StreamCtx;

// Keep the first LOGDB_CAPTURE_MAX bytes of the generated NDJSON stream for
// the request log (bounded so long generations don't bloat the database).
static void ctx_capture(StreamCtx *ctx, const char *data, size_t len)
{
    if (len == 0u || ctx->resp_len >= LOGDB_CAPTURE_MAX)
        return;
    size_t want = len;
    if (want > LOGDB_CAPTURE_MAX - ctx->resp_len)
        want = LOGDB_CAPTURE_MAX - ctx->resp_len;
    if (ctx->resp_len + want + 1u > ctx->resp_cap) {
        size_t ncap = ctx->resp_cap == 0u ? 4096u : ctx->resp_cap;
        while (ncap < ctx->resp_len + want + 1u)
            ncap *= 2u;
        char *nr = realloc(ctx->resp, ncap);
        if (nr == nullptr) {
            free(ctx->resp);
            ctx->resp = nullptr;
            ctx->resp_len = ctx->resp_cap = 0u;
            return;
        }
        ctx->resp = nr;
        ctx->resp_cap = ncap;
    }
    memcpy(ctx->resp + ctx->resp_len, data, want);
    ctx->resp_len += want;
    ctx->resp[ctx->resp_len] = '\0';
}

static void push_ndjson(StreamCtx *ctx, const char *reasoning, const char *content)
{
    char *er = json_escape(reasoning != nullptr ? reasoning : "");
    char *ec = json_escape(content != nullptr ? content : "");
    if (er == nullptr || ec == nullptr) {
        free(er);
        free(ec);
        return;
    }
    char buf[STREAM_LINE + 512u];
    if (ctx->chat)
        snprintf(buf, sizeof buf,
                 "{\"model\":\"%s\",\"created_at\":\"%s\",\"message\":{\"role\":\"assistant\",\"content\":\"%s\",\"reasoning\":\"%s\"},\"done\":false}\n",
                 ctx->req_model, ctx->created_at, ec, er);
    else
        snprintf(buf, sizeof buf,
                 "{\"model\":\"%s\",\"created_at\":\"%s\",\"response\":\"%s\",\"reasoning\":\"%s\",\"done\":false}\n",
                 ctx->req_model, ctx->created_at, ec, er);
    free(er);
    free(ec);
    lq_push(ctx->q, buf, strlen(buf));
    ctx->streamed += (long long)strlen(buf);
    ctx_capture(ctx, buf, strlen(buf));
}

static void push_done_line(StreamCtx *ctx)
{
    if (ctx->done_pushed)
        return;
    ctx->done_pushed = true;
    const long long dur = monotonic_ns() - ctx->t0_ns;
    char buf[1024];
    if (ctx->chat)
        snprintf(buf, sizeof buf,
                 "{\"model\":\"%s\",\"created_at\":\"%s\",\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"total_duration\":%lld,\"eval_count\":%ld}\n",
                 ctx->req_model, ctx->created_at, dur, ctx->eval_count);
    else
        snprintf(buf, sizeof buf,
                 "{\"model\":\"%s\",\"created_at\":\"%s\",\"response\":\"\",\"done\":true,\"total_duration\":%lld,\"eval_count\":%ld}\n",
                 ctx->req_model, ctx->created_at, dur, ctx->eval_count);
    lq_push(ctx->q, buf, strlen(buf));
    ctx->streamed += (long long)strlen(buf);
    ctx_capture(ctx, buf, strlen(buf));
}

static void handle_sse_payload(StreamCtx *ctx, const char *payload)
{
    if (strcmp(payload, "[DONE]") == 0) {
        push_done_line(ctx);
        return;
    }
    json_error_t jerr;
    json_t *root = json_loads(payload, 0, &jerr);
    if (root == nullptr)
        return;
    json_t *choices = json_object_get(root, "choices");
    if (json_is_array(choices) && json_array_size(choices) > 0u) {
        json_t *delta = json_object_get(json_array_get(choices, 0), "delta");
        // Reasoning traces: DeepSeek uses delta.reasoning_content,
        // OpenRouter/Qwen use delta.reasoning - take whichever is present so
        // bhag's traces match the provider actually handling the request.
        json_t *reasoning = json_object_get(delta, "reasoning_content");
        if (!json_is_string(reasoning))
            reasoning = json_object_get(delta, "reasoning");
        json_t *content = json_object_get(delta, "content");
        const char *rc = json_is_string(reasoning) ? json_string_value(reasoning) : nullptr;
        const char *cc = json_is_string(content) ? json_string_value(content) : nullptr;
        if (rc != nullptr || cc != nullptr) {
            ctx->eval_count++;
            push_ndjson(ctx, rc, cc);
        }
    }
    json_decref(root);
}

static void feed_sse_line(StreamCtx *ctx, const char *line)
{
    if (strncmp(line, "data:", 5u) != 0)
        return;
    const char *p = line + 5u;
    while (*p == ' ')
        p++;
    handle_sse_payload(ctx, p);
}

static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    StreamCtx *ctx = ud;
    const size_t n = size * nmemb;
    for (size_t i = 0u; i < n; ++i) {
        const char c = ptr[i];
        if (c == '\n') {
            ctx->line[ctx->line_len] = '\0';
            feed_sse_line(ctx, ctx->line);
            ctx->line_len = 0u;
        } else if (ctx->line_len + 1u < sizeof ctx->line) {
            ctx->line[ctx->line_len++] = c;
        }
    }
    return n;
}

static void *stream_thread(void *arg)
{
    StreamCtx *ctx = arg;
    char errbuf[CFG_ERRBUF_SIZE] = {0};
    long status = 0;
    if (!provider_forward_cb(ctx->prov, ctx->path, ctx->openai_body, true,
                             sse_write_cb, ctx, &status, errbuf)) {
        snprintf(ctx->error, sizeof ctx->error, "upstream error: %.200s",
                 errbuf);
        char *esc = json_escape(errbuf);
        char line[1024];
        snprintf(line, sizeof line, "{\"error\":\"%s\",\"done\":true}\n",
                 esc != nullptr ? esc : "upstream error");
        free(esc);
        lq_push(ctx->q, line, strlen(line));
        ctx->streamed += (long long)strlen(line);
        ctx_capture(ctx, line, strlen(line));
    } else if (status >= 400) {
        snprintf(ctx->error, sizeof ctx->error, "upstream HTTP %ld", status);
        char line[512];
        snprintf(line, sizeof line,
                 "{\"error\":\"upstream HTTP %ld\",\"done\":true}\n", status);
        lq_push(ctx->q, line, strlen(line));
        ctx->streamed += (long long)strlen(line);
        ctx_capture(ctx, line, strlen(line));
    } else {
        if (ctx->line_len > 0u) { // trailing line without newline
            ctx->line[ctx->line_len] = '\0';
            feed_sse_line(ctx, ctx->line);
        }
        push_done_line(ctx);
    }
    lq_close(ctx->q);
    lq_release(ctx->q);

    // Log the streamed request: the client always receives HTTP 200; failures
    // are captured in the error field.
    RequestLog rl = {0};
    snprintf(rl.protocol, sizeof rl.protocol, "%s", ctx->protocol);
    snprintf(rl.endpoint, sizeof rl.endpoint, "%s",
             ctx->orig_path[0] != '\0' ? ctx->orig_path : ctx->path);
    snprintf(rl.requested_model, sizeof rl.requested_model, "%s", ctx->req_model);
    snprintf(rl.routed_provider, sizeof rl.routed_provider, "%s", ctx->prov->name);
    snprintf(rl.routed_model, sizeof rl.routed_model, "%s", ctx->prov->model);
    rl.has_image = ctx->has_image;
    rl.stream = true;
    rl.request_size = ctx->orig_len;
    rl.request_body = ctx->orig_body;
    rl.response_size = (size_t)ctx->streamed;
    rl.status = MHD_HTTP_OK;
    rl.duration_ms = (monotonic_ns() - ctx->t0_ns) / 1000000LL;
    rl.trimmed_bytes = ctx->trimmed_bytes;
    rl.error = ctx->error[0] != '\0' ? ctx->error : nullptr;
    rl.response_body = ctx->resp; // captured up to LOGDB_CAPTURE_MAX
    logdb_record(&rl);

    free(ctx->openai_body);
    free(ctx->orig_body);
    free(ctx->resp);
    free(ctx);
    return nullptr;
}

// MHD content-reader: hand queue lines to the client as they arrive
static ssize_t ollama_reader_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    LineQueue *q = cls;
    char *line = nullptr;
    size_t len = 0u;
    if (!lq_pop(q, &line, &len))
        return MHD_CONTENT_READER_END_OF_STREAM;
    const size_t n = len < max ? len : max;
    memcpy(buf, line, n);
    free(line);
    return (ssize_t)n;
}

static void ollama_reader_free(void *cls) { lq_release(cls); }

static enum MHD_Result respond_streaming(struct MHD_Connection *conn,
                                         const Config *cfg, const char *url,
                                         bool chat, char *openai_body,
                                         const Provider *prov,
                                         const char *req_model,
                                         bool has_image,
                                         const char *orig_body,
                                         size_t orig_len,
                                         size_t trimmed_bytes)
{
    LineQueue *q = calloc(1u, sizeof *q);
    if (q == nullptr) {
        free(openai_body);
        return MHD_NO;
    }
    lq_init(q);

    StreamCtx *ctx = calloc(1u, sizeof *ctx);
    if (ctx == nullptr) {
        lq_close(q);
        lq_release(q);
        free(openai_body);
        return MHD_NO;
    }
    ctx->cfg = cfg;
    ctx->prov = prov;
    ctx->path = cfg->routing.endpoint;
    snprintf(ctx->orig_path, sizeof ctx->orig_path, "%s", url);
    ctx->openai_body = openai_body;
    ctx->chat = chat;
    snprintf(ctx->req_model, sizeof ctx->req_model, "%s", req_model);
    now_iso8601(ctx->created_at, sizeof ctx->created_at);
    ctx->q = q;
    ctx->t0_ns = monotonic_ns();
    snprintf(ctx->protocol, sizeof ctx->protocol, "%s",
             chat ? "ollama-chat" : "ollama-generate");
    ctx->has_image = has_image;
    ctx->orig_len = orig_len;
    ctx->trimmed_bytes = trimmed_bytes;
    if (orig_body != nullptr && orig_len > 0u) {
        ctx->orig_body = malloc(orig_len + 1u);
        if (ctx->orig_body != nullptr) {
            memcpy(ctx->orig_body, orig_body, orig_len);
            ctx->orig_body[orig_len] = '\0';
        }
    }

    pthread_t tid;
    if (pthread_create(&tid, nullptr, stream_thread, ctx) != 0) {
        lq_close(q);
        lq_release(q);
        free(openai_body);
        free(ctx);
        return MHD_NO;
    }
    pthread_detach(tid);

    lq_retain(q); // consumer ref
    struct MHD_Response *resp =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, NDJSON_BLOCK,
                                          &ollama_reader_cb, q,
                                          &ollama_reader_free);
    if (resp == nullptr) {
        lq_close(q);
        lq_release(q); // drop consumer ref; producer frees when done
        return MHD_NO;
    }
    MHD_add_response_header(resp, "Content-Type", "application/x-ndjson");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Access-Control-Allow-Methods",
                            "GET, POST, OPTIONS");
    MHD_add_response_header(resp, "Access-Control-Allow-Headers",
                            "Content-Type, Authorization");
    const enum MHD_Result rc = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp); // may invoke reader_free (drops consumer ref)
    return rc;
}

// ---------- non-streaming responses ----------
static bool extract_openai_content(const char *resp_body, char **content_out,
                                   char **reasoning_out, long *eval_count_out,
                                   char *errbuf)
{
    json_error_t jerr;
    json_t *root = json_loads(resp_body, 0, &jerr);
    if (root == nullptr) {
        set_err(errbuf, "upstream returned non-JSON response");
        return false;
    }
    const char *content = "";
    const char *reasoning = "";
    json_t *choices = json_object_get(root, "choices");
    if (json_is_array(choices) && json_array_size(choices) > 0u) {
        json_t *msg = json_object_get(json_array_get(choices, 0), "message");
        json_t *c = json_object_get(msg, "content");
        if (json_is_string(c))
            content = json_string_value(c);
        // DeepSeek: message.reasoning_content; OpenRouter/Qwen: message.reasoning
        json_t *r = json_object_get(msg, "reasoning_content");
        if (!json_is_string(r))
            r = json_object_get(msg, "reasoning");
        if (json_is_string(r))
            reasoning = json_string_value(r);
    }
    long eval_count = 0;
    json_t *usage = json_object_get(root, "usage");
    if (json_is_object(usage)) {
        json_t *ct = json_object_get(usage, "completion_tokens");
        if (json_is_integer(ct))
            eval_count = (long)json_integer_value(ct);
    }
    *content_out = strdup(content);
    *reasoning_out = strdup(reasoning);
    *eval_count_out = eval_count;
    json_decref(root);
    return true;
}

static char *build_ollama_response(bool chat, const char *req_model,
                                   const char *created_at, const char *content,
                                   const char *reasoning, long long dur_ns,
                                   long eval_count)
{
    json_t *r = json_object();
    json_object_set_new(r, "model", json_string(req_model));
    json_object_set_new(r, "created_at", json_string(created_at));
    if (chat) {
        json_t *msg = json_object();
        json_object_set_new(msg, "role", json_string("assistant"));
        json_object_set_new(msg, "content", json_string(content));
        if (reasoning != nullptr && reasoning[0] != '\0')
            json_object_set_new(msg, "reasoning", json_string(reasoning));
        json_object_set_new(r, "message", msg);
    } else {
        json_object_set_new(r, "response", json_string(content));
        if (reasoning != nullptr && reasoning[0] != '\0')
            json_object_set_new(r, "reasoning", json_string(reasoning));
    }
    json_object_set_new(r, "done", json_true());
    json_object_set_new(r, "total_duration", json_integer(dur_ns));
    json_object_set_new(r, "eval_count", json_integer(eval_count));
    char *s = json_dumps(r, 0);
    json_decref(r);
    return s;
}

static enum MHD_Result respond_ollama(struct MHD_Connection *conn,
                                      unsigned status, const char *body)
{
    struct MHD_Response *r = MHD_create_response_from_buffer(
        strlen(body), (void *)body, MHD_RESPMEM_MUST_COPY);
    if (r == nullptr)
        return MHD_NO;
    MHD_add_response_header(r, "Content-Type", "application/json");
    MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(r, "Access-Control-Allow-Methods",
                            "GET, POST, OPTIONS");
    MHD_add_response_header(r, "Access-Control-Allow-Headers",
                            "Content-Type, Authorization");
    const enum MHD_Result rc = MHD_queue_response(conn, status, r);
    MHD_destroy_response(r);
    return rc;
}

static enum MHD_Result respond_ollama_error(struct MHD_Connection *conn,
                                            unsigned status, const char *msg)
{
    char *esc = json_escape(msg);
    char body[1024];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}",
             esc != nullptr ? esc : "error");
    free(esc);
    return respond_ollama(conn, status, body);
}

// ---------- /api/chat and /api/generate ----------
static enum MHD_Result handle_generation(struct MHD_Connection *conn,
                                         const Config *cfg, const char *url,
                                         const char *body, size_t body_len)
{
    const bool chat = strcmp(url, "/api/chat") == 0;
    const char *protocol = chat ? "ollama-chat" : "ollama-generate";
    const long long t0 = monotonic_ns();

    RequestLog rl = {0};
    snprintf(rl.protocol, sizeof rl.protocol, "%s", protocol);
    snprintf(rl.endpoint, sizeof rl.endpoint, "%s", url);
    rl.request_size = body_len;
    rl.request_body = body;

    json_error_t jerr;
    json_t *in = json_loadb(body, body_len, 0, &jerr);
    if (in == nullptr) {
        rl.status = MHD_HTTP_BAD_REQUEST;
        rl.duration_ms = (monotonic_ns() - t0) / 1000000LL;
        rl.error = "invalid JSON body";
        logdb_record(&rl);
        return respond_ollama_error(conn, MHD_HTTP_BAD_REQUEST,
                                    "invalid JSON body");
    }

    const bool stream = json_is_true(json_object_get(in, "stream"));
    rl.stream = stream;

    // Context budget: drop the oldest /api/chat messages that don't fit
    // (same rules as the OpenAI endpoint; /api/generate has a single prompt
    // and is never trimmed).
    rl.trimmed_bytes =
        router_trim_messages(json_object_get(in, "messages"), cfg);

    const Provider *prov = nullptr;
    bool has_image = false;
    char req_model[128] = "";
    char errbuf[CFG_ERRBUF_SIZE] = {0};
    char *openai = convert_request(cfg, in, stream, !chat, &prov, &has_image,
                                   req_model, sizeof req_model, errbuf);
    json_decref(in);
    if (openai == nullptr) {
        rl.status = MHD_HTTP_BAD_REQUEST;
        rl.duration_ms = (monotonic_ns() - t0) / 1000000LL;
        rl.error = errbuf;
        logdb_record(&rl);
        return respond_ollama_error(conn, MHD_HTTP_BAD_REQUEST, errbuf);
    }
    rl.has_image = has_image;
    snprintf(rl.requested_model, sizeof rl.requested_model, "%s", req_model);
    snprintf(rl.routed_provider, sizeof rl.routed_provider, "%s", prov->name);
    snprintf(rl.routed_model, sizeof rl.routed_model, "%s", prov->model);

    if (stream)
        return respond_streaming(conn, cfg, url, chat, openai, prov, req_model,
                                 has_image, body, body_len, rl.trimmed_bytes);

    Upstream up;
    if (!provider_forward(prov, cfg->routing.endpoint, openai, false, &up,
                          errbuf)) {
        free(openai);
        rl.status = MHD_HTTP_BAD_GATEWAY;
        rl.duration_ms = (monotonic_ns() - t0) / 1000000LL;
        rl.error = errbuf;
        logdb_record(&rl);
        return respond_ollama_error(conn, MHD_HTTP_BAD_GATEWAY, errbuf);
    }
    const long long dur = monotonic_ns() - t0;
    free(openai);

    if (up.status >= 400) {
        const unsigned status =
            up.status > 0 ? (unsigned)up.status : MHD_HTTP_BAD_GATEWAY;
        char msg[512];
        snprintf(msg, sizeof msg, "upstream HTTP %ld: %.200s", up.status,
                 up.body != nullptr ? up.body : "");
        rl.status = status;
        rl.duration_ms = dur / 1000000LL;
        rl.error = msg;
        rl.response_size = up.len;
        rl.response_body = up.body;
        logdb_record(&rl);
        upstream_free(&up);
        return respond_ollama_error(conn, status, msg);
    }

    char *content = nullptr;
    char *reasoning = nullptr;
    long eval_count = 0;
    if (!extract_openai_content(up.body, &content, &reasoning, &eval_count,
                                errbuf)) {
        rl.status = MHD_HTTP_BAD_GATEWAY;
        rl.duration_ms = dur / 1000000LL;
        rl.error = errbuf;
        logdb_record(&rl);
        upstream_free(&up);
        return respond_ollama_error(conn, MHD_HTTP_BAD_GATEWAY, errbuf);
    }
    upstream_free(&up);

    char created_at[48];
    now_iso8601(created_at, sizeof created_at);
    char *resp = build_ollama_response(chat, req_model, created_at, content,
                                       reasoning, dur, eval_count);
    free(content);
    free(reasoning);
    if (resp == nullptr) {
        rl.status = MHD_HTTP_INTERNAL_SERVER_ERROR;
        rl.duration_ms = dur / 1000000LL;
        rl.error = "response build failed";
        logdb_record(&rl);
        return respond_ollama_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                    "response build failed");
    }

    rl.status = MHD_HTTP_OK;
    rl.response_size = strlen(resp);
    rl.response_body = resp;
    rl.duration_ms = dur / 1000000LL;
    logdb_record(&rl);

    const enum MHD_Result rc = respond_ollama(conn, MHD_HTTP_OK, resp);
    free(resp);
    return rc;
}

// ---------- /api/tags ----------
static char *build_tags(const Config *cfg)
{
    json_t *root = json_object();
    json_t *models = json_array();

    if (cfg->routing.ollama_model[0] != '\0') {
        // Single exported model (e.g. "bhag"): harnesses use exactly this name
        // in /api/chat and /api/generate; the router rewrites it to the
        // provider model anyway.
        json_t *m = json_object();
        json_object_set_new(m, "name", json_string(cfg->routing.ollama_model));
        json_object_set_new(m, "model", json_string(cfg->routing.ollama_model));
        json_object_set_new(m, "modified_at", json_string("2024-01-01T00:00:00Z"));
        json_object_set_new(m, "size", json_integer(0));
        json_object_set_new(m, "digest", json_string(""));
        json_object_set_new(m, "context_length",
                            json_integer((json_int_t)cfg->routing.advertised_context_length));
        json_t *details = json_object();
        json_object_set_new(details, "parent_model", json_string(""));
        json_object_set_new(details, "format", json_string("openai"));
        json_object_set_new(details, "family", json_string("openai"));
        json_object_set_new(details, "parameter_size", json_string(""));
        json_object_set_new(details, "quantization_level", json_string(""));
        json_object_set_new(details, "provider", json_string("funcroute"));
        json_object_set_new(m, "details", details);
        json_array_append_new(models, m);
    } else {
        for (size_t i = 0u; i < cfg->provider_count; ++i) {
            const Provider *p = &cfg->providers[i];
            json_t *m = json_object();
            json_object_set_new(m, "name", json_string(p->model));
            json_object_set_new(m, "model", json_string(p->model));
            json_object_set_new(m, "modified_at",
                                json_string("2024-01-01T00:00:00Z"));
            json_object_set_new(m, "size", json_integer(0));
            json_object_set_new(m, "digest", json_string(""));
            json_object_set_new(m, "context_length",
                                json_integer((json_int_t)cfg->routing.advertised_context_length));
            json_t *details = json_object();
            json_object_set_new(details, "parent_model", json_string(""));
            json_object_set_new(details, "format", json_string("openai"));
            json_object_set_new(details, "family", json_string(p->type));
            json_object_set_new(details, "parameter_size", json_string(""));
            json_object_set_new(details, "quantization_level", json_string(""));
            json_object_set_new(details, "provider", json_string(p->name));
            json_object_set_new(m, "details", details);
            json_array_append_new(models, m);
        }
    }
    json_object_set_new(root, "models", models);
    char *s = json_dumps(root, 0);
    json_decref(root);
    return s;
}

// ---------- dispatch ----------
bool ollama_handle_request(struct MHD_Connection *conn, const Config *cfg,
                           const char *url, const char *method,
                           const char *body, size_t body_len)
{
    enum MHD_Result rc = MHD_NO;

    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/version") == 0) {
        char version_json[64];
        snprintf(version_json, sizeof version_json,
                 "{\"version\":\"%s\"}", FUNCROUTE_VERSION);
        rc = respond_ollama(conn, MHD_HTTP_OK, version_json);
    } else if (strcmp(method, "GET") == 0 && strcmp(url, "/api/tags") == 0) {
        char *tags = build_tags(cfg);
        if (tags != nullptr) {
            rc = respond_ollama(conn, MHD_HTTP_OK, tags);
            free(tags);
        }
    } else if (strcmp(method, "GET") == 0 && strcmp(url, "/api/ps") == 0) {
        rc = respond_ollama(conn, MHD_HTTP_OK, "{\"models\":[]}");
    } else if (strcmp(method, "POST") == 0 && strcmp(url, "/api/show") == 0) {
        char show[1024];
        snprintf(show, sizeof show,
                 "{\"license\":\"funcroute\",\"modelfile\":\"\","
                 "\"parameters\":\"\",\"template\":\"\","
                 "\"context_length\":%d,"
                 "\"model_info\":{"
                 "\"general.architecture\":\"deepseek\","
                 "\"general.parameter_count\":0,"
                 "\"deepseek.context_length\":%d,"
                 "\"llama.context_length\":%d},"
                 "\"details\":{\"format\":\"openai\",\"family\":\"\","
                 "\"parameter_size\":\"\",\"quantization_level\":\"\"}}",
                 (int)cfg->routing.advertised_context_length,
                 (int)cfg->routing.advertised_context_length,
                 (int)cfg->routing.advertised_context_length);
        rc = respond_ollama(conn, MHD_HTTP_OK, show);
    } else if (strcmp(method, "POST") == 0 &&
               (strcmp(url, "/api/chat") == 0 ||
                strcmp(url, "/api/generate") == 0)) {
        rc = handle_generation(conn, cfg, url, body, body_len);
    } else if (strcmp(method, "POST") == 0 &&
               strcmp(url, "/api/embeddings") == 0) {
        rc = respond_ollama_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "embeddings are not supported by funcroute");
    } else {
        return false; // unknown endpoint -> caller answers 404
    }
    return rc == MHD_YES;
}
