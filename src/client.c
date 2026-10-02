// client.c - OpenAI-compatible CLI frontend for funcroute (or any
// /v1/chat/completions endpoint). Sends text and/or image requests,
// prints the assistant reply, supports SSE streaming.
//
// Deps: libcurl (HTTP), jansson (JSON), openssl (base64 for image files).
// C23 build: gcc -std=c2x
#include "config.h"

#include <curl/curl.h>
#include <jansson.h>
#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#define MAX_MEDIA      8u        // per-kind cap (images, audio, files)
#define MAX_PROMPT_LEN (1u << 20)  // 1 MiB inline prompt / text file cap
#define MAX_ATTACH_LEN (32u << 20) // 32 MiB attachment (image/audio/pdf) cap
#define ERRBUF_SIZE    512u

typedef struct {
    char base_url[256];
    char endpoint[64];
    char model[128];
    char *prompt;            // malloc'd user prompt
    char *images[MAX_MEDIA]; // image URLs or file paths
    size_t image_count;
    char *audios[MAX_MEDIA]; // audio file paths (or data: URLs)
    size_t audio_count;
    char *files[MAX_MEDIA];  // file/PDF paths (or data: URLs)
    size_t file_count;
    bool stream;
    bool ollama;             // speak /api/chat (Ollama protocol) instead
    long timeout_secs;
} Options;

static void die(const char *fmt, ...) __attribute__((noreturn));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void usage(const char *prog)
{
    printf(
        "usage: %s [options]\n"
        "OpenAI-compatible frontend for funcroute.\n\n"
        "  -c, --config PATH     config file (default: client.json)\n"
        "  -b, --base-url URL    router base URL (default: http://127.0.0.1:8080)\n"
        "  -e, --endpoint PATH   API path appended to base URL (default: /v1/chat/completions)\n"
        "  -m, --model NAME      model to request (router rewrites it anyway)\n"
        "  -p, --prompt TEXT     user prompt (inline)\n"
        "  -f, --file PATH       read the prompt from a text file\n"
        "  -i, --image URL|PATH  attach an image; URL or local file (repeatable).\n"
        "                        Local files are sent as data: URLs (base64).\n"
        "  -a, --audio PATH      attach audio (wav/mp3/... ; repeatable). Sent as\n"
        "                        an OpenAI input_audio part (raw base64 + format).\n"
        "  -A, --attach PATH     attach a file such as a PDF (repeatable). Sent as\n"
        "                        an OpenRouter-style file part with a data URL.\n"
        "  -s, --stream          stream tokens as they arrive (SSE)\n"
        "  -o, --ollama          talk to the Ollama API (/api/chat) instead of\n"
        "                        /v1/chat/completions (e.g. funcroute's Ollama layer)\n"
        "  -t, --timeout SECS    request timeout\n"
        "  -h, --help            show this help\n",
        prog);
}

// ---------- tiny byte buffer for curl callbacks ----------
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static bool buf_append(Buf *b, const char *p, size_t n)
{
    if (n == 0u)
        return true;
    if (b->len + n + 1u > b->cap) {
        size_t ncap = b->cap == 0u ? 4096u : b->cap;
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

// ---------- config ----------
static void load_config(Options *o, const char *path)
{
    json_error_t jerr;
    json_t *root = json_load_file(path, 0, &jerr);
    if (root == nullptr)
        return; // missing config is fine, defaults/CLI win
    if (!json_is_object(root)) {
        json_decref(root);
        return;
    }
    json_t *v;
    if ((v = json_object_get(root, "base_url")) && json_is_string(v))
        snprintf(o->base_url, sizeof o->base_url, "%s", json_string_value(v));
    if ((v = json_object_get(root, "endpoint")) && json_is_string(v))
        snprintf(o->endpoint, sizeof o->endpoint, "%s", json_string_value(v));
    if ((v = json_object_get(root, "model")) && json_is_string(v))
        snprintf(o->model, sizeof o->model, "%s", json_string_value(v));
    if ((v = json_object_get(root, "stream")) && json_is_boolean(v))
        o->stream = json_is_true(v);
    if ((v = json_object_get(root, "timeout_secs")) && json_is_integer(v))
        o->timeout_secs = (long)json_integer_value(v);
    json_decref(root);
}

// ---------- file helpers ----------
static char *read_whole_file(const char *path, size_t max, size_t *out_len,
                             char *errbuf, size_t errlen)
{
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        snprintf(errbuf, errlen, "cannot open %s", path);
        return nullptr;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        snprintf(errbuf, errlen, "cannot seek %s", path);
        return nullptr;
    }
    const long sz = ftell(fp);
    if (sz < 0 || (unsigned long)sz > max) {
        fclose(fp);
        snprintf(errbuf, errlen, "%s too large (max %zu bytes)", path, max);
        return nullptr;
    }
    rewind(fp);
    char *data = malloc((size_t)sz + 1u);
    if (data == nullptr) {
        fclose(fp);
        snprintf(errbuf, errlen, "out of memory");
        return nullptr;
    }
    const size_t got = fread(data, 1u, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(data);
        snprintf(errbuf, errlen, "short read on %s", path);
        return nullptr;
    }
    data[got] = '\0';
    if (out_len != nullptr)
        *out_len = got;
    return data;
}

static const char *mime_by_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr)
        return "application/octet-stream";
    // images
    if (strcasecmp(dot, ".png") == 0)
        return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcasecmp(dot, ".webp") == 0)
        return "image/webp";
    // audio
    if (strcasecmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcasecmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".mp4") == 0)
        return "audio/mp4";
    if (strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".oga") == 0 ||
        strcasecmp(dot, ".opus") == 0)
        return "audio/ogg";
    if (strcasecmp(dot, ".flac") == 0)
        return "audio/flac";
    if (strcasecmp(dot, ".aac") == 0)
        return "audio/aac";
    if (strcasecmp(dot, ".webm") == 0)
        return "audio/webm";
    // documents / other files
    if (strcasecmp(dot, ".pdf") == 0)
        return "application/pdf";
    if (strcasecmp(dot, ".txt") == 0)
        return "text/plain";
    if (strcasecmp(dot, ".md") == 0)
        return "text/markdown";
    if (strcasecmp(dot, ".csv") == 0)
        return "text/csv";
    if (strcasecmp(dot, ".json") == 0)
        return "application/json";
    return "application/octet-stream";
}

// Audio "format" for OpenAI input_audio (wav, mp3, ...), from the extension.
static const char *audio_format(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr)
        return "wav";
    if (strcasecmp(dot, ".wav") == 0)
        return "wav";
    if (strcasecmp(dot, ".mp3") == 0)
        return "mp3";
    if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".mp4") == 0)
        return "m4a";
    if (strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".oga") == 0)
        return "ogg";
    if (strcasecmp(dot, ".opus") == 0)
        return "opus";
    if (strcasecmp(dot, ".flac") == 0)
        return "flac";
    if (strcasecmp(dot, ".aac") == 0)
        return "aac";
    if (strcasecmp(dot, ".webm") == 0)
        return "webm";
    return "wav";
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

// Local file -> "data:<mime>;base64,..." via OpenSSL EVP_EncodeBlock.
// URLs (http/https/data:) are passed through untouched.
static char *image_to_url(const char *src, char *errbuf, size_t errlen)
{
    if (strncmp(src, "http://", 7u) == 0 || strncmp(src, "https://", 8u) == 0 ||
        strncmp(src, "data:", 5u) == 0)
        return strdup(src); // already a URL / data URL

    size_t raw_len = 0u;
    char *raw = read_whole_file(src, MAX_ATTACH_LEN, &raw_len, errbuf, errlen);
    if (raw == nullptr)
        return nullptr;

    const size_t b64_len = 4u * ((raw_len + 2u) / 3u) + 1u;
    char *b64 = malloc(b64_len);
    if (b64 == nullptr) {
        free(raw);
        snprintf(errbuf, errlen, "out of memory");
        return nullptr;
    }
    const int wrote = EVP_EncodeBlock((unsigned char *)b64,
                                      (const unsigned char *)raw, (int)raw_len);
    free(raw);
    if (wrote <= 0) {
        free(b64);
        snprintf(errbuf, errlen, "base64 encode failed");
        return nullptr;
    }
    b64[wrote] = '\0';

    const size_t need = 64u + strlen(mime_by_ext(src)) + strlen(b64);
    char *url = malloc(need);
    if (url == nullptr) {
        free(b64);
        snprintf(errbuf, errlen, "out of memory");
        return nullptr;
    }
    snprintf(url, need, "data:%s;base64,%s", mime_by_ext(src), b64);
    free(b64);
    return url;
}

// OpenAI audio part: {"type":"input_audio","input_audio":{"data":<b64>,"format":..}}
// Attachments arrive inline (data URL) from a capture stream; a local file
// path is also accepted for convenience.
static json_t *build_audio_part(const char *src, char *errbuf, size_t errlen)
{
    char format[64] = "wav";
    char *b64 = nullptr;

    if (strncmp(src, "data:", 5u) == 0) {
        const char *comma = strstr(src, ";base64,");
        if (comma == nullptr) {
            snprintf(errbuf, errlen, "audio data URL must be base64-encoded");
            return nullptr;
        }
        const char *mime = src + 5u;
        const size_t mlen = (size_t)(comma - mime);
        if (mlen > 0u && mlen < 64u) {
            char m[64];
            memcpy(m, mime, mlen);
            m[mlen] = '\0';
            const char *slash = strchr(m, '/');
            const char *sub = (slash != nullptr && slash[1] != '\0') ? slash + 1
                                                                     : m;
            if (strcmp(sub, "mpeg") == 0)
                snprintf(format, sizeof format, "%s", "mp3");
            else if (strcmp(sub, "mp4") == 0)
                snprintf(format, sizeof format, "%s", "m4a");
            else
                snprintf(format, sizeof format, "%s", sub);
        }
        b64 = strdup(comma + 8u);
    } else if (strncmp(src, "http://", 7u) == 0 ||
               strncmp(src, "https://", 8u) == 0) {
        snprintf(errbuf, errlen,
                 "audio must be a local file or data: URL (got \"%s\")", src);
        return nullptr;
    } else {
        size_t raw_len = 0u;
        char *raw = read_whole_file(src, MAX_ATTACH_LEN, &raw_len, errbuf, errlen);
        if (raw == nullptr)
            return nullptr;
        const size_t need = 4u * ((raw_len + 2u) / 3u) + 1u;
        b64 = malloc(need);
        if (b64 != nullptr) {
            const int wrote = EVP_EncodeBlock((unsigned char *)b64,
                                              (const unsigned char *)raw,
                                              (int)raw_len);
            if (wrote <= 0)
                b64 = (free(b64), nullptr);
            else {
                b64[wrote] = '\0';
                snprintf(format, sizeof format, "%s", audio_format(src));
            }
        }
        free(raw);
    }
    if (b64 == nullptr) {
        if (errbuf[0] == '\0')
            snprintf(errbuf, errlen, "out of memory");
        return nullptr;
    }

    json_t *part = json_object();
    json_object_set_new(part, "type", json_string("input_audio"));
    json_t *ia = json_object();
    json_object_set_new(ia, "data", json_string(b64));
    json_object_set_new(ia, "format", json_string(format));
    json_object_set_new(part, "input_audio", ia);
    free(b64);
    return part;
}

// OpenRouter-style file part:
// {"type":"file","file":{"filename":<name>,"file_data":<data URL>}}
static json_t *build_file_part(const char *src, char *errbuf, size_t errlen)
{
    char *data_url = nullptr;
    char fname[256];

    if (strncmp(src, "data:", 5u) == 0) {
        data_url = strdup(src);
        const char *mime = src + 5u;
        const char *semi = strchr(mime, ';');
        const size_t mlen = semi != nullptr ? (size_t)(semi - mime) : strlen(mime);
        char m[64] = "application/octet-stream";
        if (mlen > 0u && mlen < sizeof m) {
            memcpy(m, mime, mlen);
            m[mlen] = '\0';
        }
        const char *slash = strchr(m, '/');
        const char *ext = (slash != nullptr && slash[1] != '\0') ? slash + 1
                                                                 : "bin";
        if (strcmp(ext, "plain") == 0)
            ext = "txt";
        else if (strcmp(ext, "jpeg") == 0)
            ext = "jpg";
        snprintf(fname, sizeof fname, "attachment.%s", ext);
    } else if (strncmp(src, "http://", 7u) == 0 ||
               strncmp(src, "https://", 8u) == 0) {
        snprintf(errbuf, errlen,
                 "file attachments must be local files or data: URLs (got \"%s\")",
                 src);
        return nullptr;
    } else {
        data_url = image_to_url(src, errbuf, errlen); // data:<mime>;base64,...
        if (data_url == nullptr)
            return nullptr;
        snprintf(fname, sizeof fname, "%s", basename_of(src));
    }
    if (data_url == nullptr) {
        snprintf(errbuf, errlen, "out of memory");
        return nullptr;
    }

    json_t *part = json_object();
    json_object_set_new(part, "type", json_string("file"));
    json_t *fo = json_object();
    json_object_set_new(fo, "filename", json_string(fname));
    json_object_set_new(fo, "file_data", json_string(data_url));
    json_object_set_new(part, "file", fo);
    free(data_url);
    return part;
}

// ---------- response handling ----------
static void print_choice_message(json_t *root) // non-streaming reply
{
    json_t *choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0u)
        return;
    json_t *msg = json_object_get(json_array_get(choices, 0), "message");
    json_t *content = json_object_get(msg, "content");
    if (json_is_string(content))
        fputs(json_string_value(content), stdout);
}

static void handle_sse_line(const char *line)
{
    if (strncmp(line, "data:", 5u) != 0)
        return;
    const char *payload = line + 5u;
    while (*payload == ' ')
        payload++;
    if (strcmp(payload, "[DONE]") == 0)
        return;

    json_error_t jerr;
    json_t *root = json_loads(payload, 0, &jerr);
    if (root == nullptr)
        return;
    json_t *choices = json_object_get(root, "choices");
    if (json_is_array(choices) && json_array_size(choices) > 0u) {
        json_t *delta = json_object_get(json_array_get(choices, 0), "delta");
        json_t *content = json_object_get(delta, "content");
        if (json_is_string(content)) {
            fputs(json_string_value(content), stdout);
            fflush(stdout);
        }
    }
    json_decref(root);
}

// Ollama NDJSON line: {"message":{"content":...},"done":...} or {"response":...}
static void handle_ndjson_line(const char *line)
{
    if (line[0] == '\0')
        return;
    json_error_t jerr;
    json_t *root = json_loads(line, 0, &jerr);
    if (root == nullptr)
        return;
    json_t *content = nullptr;
    json_t *msg = json_object_get(root, "message");
    if (json_is_object(msg))
        content = json_object_get(msg, "content");
    if (!json_is_string(content))
        content = json_object_get(root, "response");
    if (json_is_string(content)) {
        fputs(json_string_value(content), stdout);
        fflush(stdout);
    }
    json_decref(root);
}

// curl write callback for SSE/NDJSON: buffer lines, print payloads live
typedef struct {
    char line[8192];
    size_t line_len;
    bool ollama;
} Sse;

static size_t stream_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Sse *s = ud;
    const size_t n = size * nmemb;
    for (size_t i = 0u; i < n; ++i) {
        const char c = ptr[i];
        if (c == '\n') {
            s->line[s->line_len] = '\0';
            if (s->ollama)
                handle_ndjson_line(s->line);
            else
                handle_sse_line(s->line);
            s->line_len = 0u;
        } else if (s->line_len + 1u < sizeof s->line) {
            s->line[s->line_len++] = c;
        }
    }
    return n;
}

static size_t collect_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Buf *b = ud;
    return buf_append(b, ptr, size * nmemb) ? size * nmemb : 0u;
}

// ---------- request ----------
static char *build_request(const Options *o, char *errbuf, size_t errlen)
{
    json_t *root = json_object();
    json_t *messages = json_array();
    json_t *user = json_object();
    json_object_set_new(user, "role", json_string("user"));

    const bool has_media = o->image_count > 0u || o->audio_count > 0u ||
                           o->file_count > 0u;
    if (has_media) {
        json_t *content = json_array();
        json_t *text_part = json_object();
        json_object_set_new(text_part, "type", json_string("text"));
        json_object_set_new(text_part, "text",
                            json_string(o->prompt != nullptr ? o->prompt : ""));
        json_array_append_new(content, text_part);
        for (size_t i = 0u; i < o->image_count; ++i) {
            json_t *part = json_object();
            json_object_set_new(part, "type", json_string("image_url"));
            json_t *iu = json_object();
            json_object_set_new(iu, "url", json_string(o->images[i]));
            json_object_set_new(part, "image_url", iu);
            json_array_append_new(content, part);
        }
        for (size_t i = 0u; i < o->audio_count; ++i) {
            json_t *part = build_audio_part(o->audios[i], errbuf, errlen);
            if (part == nullptr) {
                json_decref(content);
                json_decref(user);
                json_decref(messages);
                json_decref(root);
                return nullptr;
            }
            json_array_append_new(content, part);
        }
        for (size_t i = 0u; i < o->file_count; ++i) {
            json_t *part = build_file_part(o->files[i], errbuf, errlen);
            if (part == nullptr) {
                json_decref(content);
                json_decref(user);
                json_decref(messages);
                json_decref(root);
                return nullptr;
            }
            json_array_append_new(content, part);
        }
        json_object_set_new(user, "content", content);
    } else {
        json_object_set_new(user, "content",
                            json_string(o->prompt != nullptr ? o->prompt : ""));
    }
    json_array_append_new(messages, user);

    json_object_set_new(root, "model", json_string(o->model));
    json_object_set_new(root, "messages", messages);
    json_object_set_new(root, "stream", json_boolean(o->stream));

    char *out = json_dumps(root, 0);
    json_decref(root);
    if (out == nullptr)
        snprintf(errbuf, errlen, "request serialization failed");
    return out;
}

// Ollama /api/chat request: images become raw base64 in message "images".
static char *build_request_ollama(const Options *o, char *errbuf, size_t errlen)
{
    if (o->audio_count > 0u || o->file_count > 0u) {
        set_err(errbuf,
                "ollama mode supports images only (no audio/file attachments)");
        return nullptr;
    }
    json_t *root = json_object();
    json_t *messages = json_array();
    json_t *user = json_object();
    json_object_set_new(user, "role", json_string("user"));
    json_object_set_new(user, "content",
                        json_string(o->prompt != nullptr ? o->prompt : ""));

    json_t *images = json_array();
    for (size_t i = 0u; i < o->image_count; ++i) {
        const char *src = o->images[i];
        if (strncmp(src, "data:", 5u) == 0) {
            const char *comma = strstr(src, ";base64,");
            if (comma != nullptr)
                json_array_append_new(images,
                                      json_string(comma + 8u)); // raw base64
        } else {
            set_err(errbuf, "ollama mode needs local image files (got \"%s\")",
                    src);
            json_decref(images);
            json_decref(user);
            json_decref(messages);
            json_decref(root);
            return nullptr;
        }
    }
    if (json_array_size(images) > 0u)
        json_object_set_new(user, "images", images);
    else
        json_decref(images);
    json_array_append_new(messages, user);

    json_object_set_new(root, "model", json_string(o->model));
    json_object_set_new(root, "messages", messages);
    json_object_set_new(root, "stream", json_boolean(o->stream));

    char *out = json_dumps(root, 0);
    json_decref(root);
    if (out == nullptr)
        snprintf(errbuf, errlen, "request serialization failed");
    return out;
}

static int run(const Options *o)
{
    char errbuf[ERRBUF_SIZE] = {0};
    char *body = o->ollama ? build_request_ollama(o, errbuf, sizeof errbuf)
                           : build_request(o, errbuf, sizeof errbuf);
    if (body == nullptr)
        die("error: %s", errbuf);

    char url[512];
    if (o->ollama)
        snprintf(url, sizeof url, "%s/api/chat", o->base_url);
    else
        snprintf(url, sizeof url, "%s%s", o->base_url, o->endpoint);

    CURL *curl = curl_easy_init();
    if (curl == nullptr)
        die("error: curl_easy_init failed");

    Sse sse = {.ollama = o->ollama};
    Buf collected = {0};
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs,
                             o->stream ? "Accept: text/event-stream"
                                       : "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    if (o->stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &collected);
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, o->timeout_secs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        die("error: request to %s failed: %s", url, curl_easy_strerror(rc));

    if (status < 200 || status >= 300) {
        fprintf(stderr, "error: HTTP %ld: %s\n", status,
                collected.data != nullptr ? collected.data : "(no body)");
        free(collected.data);
        free(body);
        return EXIT_FAILURE;
    }

    if (!o->stream && collected.data != nullptr) {
        json_error_t jerr;
        json_t *root = json_loads(collected.data, 0, &jerr);
        if (root != nullptr) {
            if (o->ollama) {
                const char *content = nullptr;
                json_t *msg = json_object_get(root, "message");
                if (json_is_object(msg))
                    content = json_string_value(json_object_get(msg, "content"));
                if (content == nullptr)
                    content = json_string_value(json_object_get(root, "response"));
                if (content == nullptr)
                    content = json_string_value(json_object_get(root, "error"));
                if (content != nullptr)
                    fputs(content, stdout);
            } else {
                print_choice_message(root);
            }
            json_decref(root);
            fputc('\n', stdout);
        } else {
            fputs(collected.data, stdout); // not JSON: pass through raw
        }
    }
    if (o->stream)
        fputc('\n', stdout);

    free(collected.data);
    free(body);
    return EXIT_SUCCESS;
}

// ---------- main ----------
int main(int argc, char **argv)
{
    Options o = {
        .base_url = "http://127.0.0.1:8080",
        .endpoint = "/v1/chat/completions",
        .model = "client-model",
        .timeout_secs = 120,
    };

    // Pass 1: locate the config file so CLI options can override it.
    const char *config_path = "client.json";
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strncmp(a, "--config=", 9u) == 0) {
            config_path = a + 9u;
        }
    }
    load_config(&o, config_path);

    // Pass 2: apply CLI options on top of config (CLI always wins).
    const char *prompt_file = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *val = nullptr;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
        // support "--opt value" and "--opt=value"
        char opt[64];
        if (a[0] == '-' && a[1] == '-') {
            const char *eq = strchr(a, '=');
            if (eq != nullptr) {
                const size_t len = (size_t)(eq - a);
                if (len >= sizeof opt)
                    die("error: option too long: %s", a);
                memcpy(opt, a, len);
                opt[len] = '\0';
                val = eq + 1;
            } else {
                snprintf(opt, sizeof opt, "%s", a);
            }
        } else {
            snprintf(opt, sizeof opt, "%s", a);
        }
        // Only options that take a value consume the next argument.
        const bool takes_value =
            strcmp(opt, "-c") == 0 || strcmp(opt, "--config") == 0 ||
            strcmp(opt, "-b") == 0 || strcmp(opt, "--base-url") == 0 ||
            strcmp(opt, "-e") == 0 || strcmp(opt, "--endpoint") == 0 ||
            strcmp(opt, "-m") == 0 || strcmp(opt, "--model") == 0 ||
            strcmp(opt, "-p") == 0 || strcmp(opt, "--prompt") == 0 ||
            strcmp(opt, "-f") == 0 || strcmp(opt, "--file") == 0 ||
            strcmp(opt, "-i") == 0 || strcmp(opt, "--image") == 0 ||
            strcmp(opt, "-a") == 0 || strcmp(opt, "--audio") == 0 ||
            strcmp(opt, "-A") == 0 || strcmp(opt, "--attach") == 0 ||
            strcmp(opt, "-t") == 0 || strcmp(opt, "--timeout") == 0;
        if (takes_value && val == nullptr && i + 1 < argc)
            val = argv[++i];

        if (strcmp(opt, "-c") == 0 || strcmp(opt, "--config") == 0)
            config_path = val;
        else if (strcmp(opt, "-b") == 0 || strcmp(opt, "--base-url") == 0)
            snprintf(o.base_url, sizeof o.base_url, "%s", val);
        else if (strcmp(opt, "-e") == 0 || strcmp(opt, "--endpoint") == 0)
            snprintf(o.endpoint, sizeof o.endpoint, "%s", val);
        else if (strcmp(opt, "-m") == 0 || strcmp(opt, "--model") == 0)
            snprintf(o.model, sizeof o.model, "%s", val);
        else if (strcmp(opt, "-p") == 0 || strcmp(opt, "--prompt") == 0) {
            if (o.prompt != nullptr)
                free(o.prompt);
            o.prompt = strdup(val);
        } else if (strcmp(opt, "-f") == 0 || strcmp(opt, "--file") == 0)
            prompt_file = val;
        else if (strcmp(opt, "-i") == 0 || strcmp(opt, "--image") == 0) {
            if (o.image_count >= MAX_MEDIA)
                die("error: too many images (max %u)", MAX_MEDIA);
            o.images[o.image_count++] = (char *)val;
        } else if (strcmp(opt, "-a") == 0 || strcmp(opt, "--audio") == 0) {
            if (o.audio_count >= MAX_MEDIA)
                die("error: too many audio attachments (max %u)", MAX_MEDIA);
            o.audios[o.audio_count++] = (char *)val;
        } else if (strcmp(opt, "-A") == 0 || strcmp(opt, "--attach") == 0) {
            if (o.file_count >= MAX_MEDIA)
                die("error: too many file attachments (max %u)", MAX_MEDIA);
            o.files[o.file_count++] = (char *)val;
        } else if (strcmp(opt, "-s") == 0 || strcmp(opt, "--stream") == 0)
            o.stream = true;
        else if (strcmp(opt, "-o") == 0 || strcmp(opt, "--ollama") == 0)
            o.ollama = true;
        else if (strcmp(opt, "-t") == 0 || strcmp(opt, "--timeout") == 0)
            o.timeout_secs = strtol(val, nullptr, 10);
        else
            die("error: unknown option %s (try -h)", a);
    }

    if (prompt_file != nullptr) {
        char errbuf[ERRBUF_SIZE] = {0};
        char *text = read_whole_file(prompt_file, MAX_PROMPT_LEN, nullptr,
                                     errbuf, sizeof errbuf);
        if (text == nullptr)
            die("error: %s", errbuf);
        if (o.prompt != nullptr)
            free(o.prompt);
        o.prompt = text;
    }
    if (o.prompt == nullptr && o.image_count == 0u && o.audio_count == 0u &&
        o.file_count == 0u)
        die("error: nothing to send (use -p/--prompt, -f/--file, -i/--image, "
            "-a/--audio or -A/--attach)");

    // Convert local image paths to data URLs (URLs pass through).
    char errbuf[ERRBUF_SIZE] = {0};
    char *resolved[MAX_MEDIA] = {nullptr};
    for (size_t i = 0u; i < o.image_count; ++i) {
        resolved[i] = image_to_url(o.images[i], errbuf, sizeof errbuf);
        if (resolved[i] == nullptr)
            die("error: image %s: %s", o.images[i], errbuf);
    }
    for (size_t i = 0u; i < o.image_count; ++i)
        o.images[i] = resolved[i];

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
        die("error: curl_global_init failed");
    const int rc = run(&o);
    curl_global_cleanup();

    for (size_t i = 0u; i < o.image_count; ++i)
        free(resolved[i]);
    free(o.prompt);
    return rc;
}
