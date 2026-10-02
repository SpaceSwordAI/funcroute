// config.h - typed view of config.json (parsed with jansson)
// C23 build: gcc -std=c2x
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

// Version stamped in by the build (`make VERSION=1.2.3`), reported by
// GET /api/version and printed in the startup banner. A hand-compiled binary
// that skips the Makefile reports "dev" rather than claiming a release number.
#ifndef FUNCROUTE_VERSION
#define FUNCROUTE_VERSION "dev"
#endif

// Default context window advertised to harnesses (/v1/models, /api/tags,
// /api/show) when routing.advertised_context_length is not set. Text requests
// route to DeepSeek, whose real context is 128K tokens; harnesses display
// this number as the context-window meter. The routing.context budget
// (max_request_bytes) is what actually bounds traffic upstream.
#define DEFAULT_ADVERTISED_CONTEXT 131072

// Fixed-size string limits (simple, stack-friendly config struct)
#define CFG_MAX_PROVIDERS    8u
#define CFG_STR_NAME         32u
#define CFG_STR_URL          256u
#define CFG_STR_KEY          512u
#define CFG_STR_MODEL        128u
#define CFG_STR_HOST         64u
#define CFG_STR_ENDPOINT     64u
#define CFG_ERRBUF_SIZE      256u

static_assert(CFG_MAX_PROVIDERS > 1u, "need room for at least two providers");

typedef struct {
    char name[CFG_STR_NAME];
    char type[CFG_STR_NAME];   // provider type, e.g. "openai" (reserved for future)
    char base_url[CFG_STR_URL];// API root, e.g. "https://api.deepseek.com"
    char api_key[CFG_STR_KEY]; // resolved: config "api_key" or env var "api_key_env"
    char model[CFG_STR_MODEL]; // model forced onto routed requests
    char reasoning[16];        // optional "low"|"medium"|"high" -> reasoning_effort
    long timeout_secs;         // libcurl timeout
} Provider;

typedef struct {
    char          host[CFG_STR_HOST];
    uint16_t      port;
    unsigned      max_connections;
} ServerSettings;

typedef struct {
    // Attachment routing is per *kind*, because upstream models differ in what
    // they accept: DeepSeek VL reads images and PDFs but rejects audio, while
    // an omni/Gemini model is needed for audio. A request may carry several
    // kinds at once, so the most restrictive kind wins (audio > file > image):
    // the selected provider must cope with everything in the request.
    char default_provider[CFG_STR_NAME]; // used for text-only requests
    char image_provider[CFG_STR_NAME];   // provider for image_url parts
    char audio_provider[CFG_STR_NAME];   // provider for input_audio parts;
                                         // falls back to media_provider
    char file_provider[CFG_STR_NAME];    // provider for file/PDF parts;
                                         // falls back to media_provider
    char media_provider[CFG_STR_NAME];   // catch-all for any attachment part;
                                         // falls back to image_provider
    char image_content_type[CFG_STR_NAME]; // content part type, e.g. "image_url"
    char audio_content_type[CFG_STR_NAME]; // content part type, e.g. "input_audio"
    char file_content_type[CFG_STR_NAME];  // content part type, e.g. "file"
    char endpoint[CFG_STR_ENDPOINT];     // e.g. "/v1/chat/completions"
    char ollama_model[CFG_STR_MODEL];    // single model name exposed via /api/tags
                                         // (empty = list all providers instead)
    size_t max_request_bytes;            // context budget: drop oldest messages
                                         // until the request fits (0 = disabled)
    size_t max_messages;                 // hard cap on messages per request
                                         // (0 = disabled)
    size_t advertised_context_length;    // context window shown to harnesses in
                                         // /v1/models, /api/tags, /api/show
} Routing;

typedef struct {
    ServerSettings server;
    Routing        routing;
    Provider       providers[CFG_MAX_PROVIDERS];
    size_t         provider_count;
    char           db_path[CFG_STR_URL]; // SQLite request log path ("" = disabled)
} Config;

// Format a message into an error buffer (shared by config.c / provider.c)
[[gnu::format(printf, 2, 3)]] static inline void
set_err(char errbuf[CFG_ERRBUF_SIZE], const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, CFG_ERRBUF_SIZE, fmt, ap);
    va_end(ap);
}

// Load + validate config.json into cfg. Returns false and fills errbuf on error.
[[nodiscard]] bool config_load(Config *cfg, const char *path,
                               char errbuf[CFG_ERRBUF_SIZE]);

// Find a provider by name; nullptr if absent.
[[nodiscard]] const Provider *config_find(const Config *cfg, const char *name);
