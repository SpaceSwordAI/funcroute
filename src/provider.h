// provider.h - upstream HTTP forwarding via libcurl
#pragma once

#include "config.h"

#include <stddef.h>

typedef struct {
    long   status;         // HTTP status code from upstream
    char  *body;           // malloc'd response body (NUL-terminated)
    size_t len;            // bytes in body
    char   content_type[128]; // captured Content-Type header, "" if absent
} Upstream;

// Caller-supplied libcurl write callback (size*nmemb bytes of body data).
typedef size_t (*provider_write_fn)(char *ptr, size_t size, size_t nmemb,
                                    void *ud);

// Like provider_forward, but delivers the response body incrementally through
// wcb(wud) instead of buffering it (used for streaming conversions).
// The upstream HTTP status is returned via *status_out.
[[nodiscard]] bool provider_forward_cb(const Provider *p, const char *path,
                                       const char *json_body, bool stream,
                                       provider_write_fn wcb, void *wud,
                                       long *status_out,
                                       char errbuf[CFG_ERRBUF_SIZE]);

// POST json_body to p->base_url + path with Bearer auth.
// stream=true requests "Accept: text/event-stream" so SSE passes through.
// Returns false (errbuf filled) on transport failure; HTTP error statuses
// from the upstream are NOT a failure - they land in up->status with the body.
[[nodiscard]] bool provider_forward(const Provider *p, const char *path,
                                    const char *json_body, bool stream,
                                    Upstream *up, char errbuf[CFG_ERRBUF_SIZE]);

void upstream_free(Upstream *up);
