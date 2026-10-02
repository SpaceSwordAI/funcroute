// ollama.h - Ollama-compatible API layer for funcroute
//
// Presents the router as an Ollama server (/api/version, /api/tags, /api/ps,
// /api/show, /api/chat, /api/generate) so coding harnesses that speak the
// Ollama protocol can use funcroute unchanged. Requests are converted to
// OpenAI format and routed by image presence (same deepseek/openrouter
// routing as /v1/chat/completions).
#pragma once

#include "config.h"

#include <jansson.h>
#include <microhttpd.h>

// Handle an /api/* request. Returns true if a response was queued
// (including error responses); false if the endpoint is unknown, in which
// case the caller should answer 404.
bool ollama_handle_request(struct MHD_Connection *conn, const Config *cfg,
                           const char *url, const char *method,
                           const char *body, size_t body_len);

// Apply the configured context budget (routing.max_request_bytes /
// routing.max_messages) to an OpenAI-format messages array in place: drops
// oldest messages, always keeping system messages, the last message and any
// image-bearing message; image payload bytes are excluded from the byte
// budget. Returns bytes dropped (0 = unchanged).
size_t router_trim_messages(json_t *messages, const Config *cfg);
