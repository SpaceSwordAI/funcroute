// provider.c - forward a JSON payload to an upstream OpenAI-compatible API
#include "provider.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// libcurl write callback: append into Upstream->body
static size_t collect_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Upstream *up = ud;
    const size_t n = size * nmemb;
    char *nb = realloc(up->body, up->len + n + 1u);
    if (nb == nullptr)
        return 0u; // abort transfer
    up->body = nb;
    memcpy(up->body + up->len, ptr, n);
    up->len += n;
    up->body[up->len] = '\0';
    return n;
}

// libcurl header callback: keep the Content-Type of the upstream reply
static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Upstream *up = ud;
    const size_t n = size * nmemb;
    static const char prefix[] = "Content-Type:";
    if (n >= sizeof prefix && strncasecmp(ptr, prefix, sizeof prefix - 1u) == 0) {
        const char *start = ptr + sizeof prefix - 1u;
        while (*start == ' ' || *start == '\t')
            start++;
        size_t len = 0u;
        while (start[len] != '\0' && start[len] != '\r' && start[len] != '\n')
            len++;
        if (len > 0u && len < sizeof up->content_type) {
            memcpy(up->content_type, start, len);
            up->content_type[len] = '\0';
        }
    }
    return n;
}

void upstream_free(Upstream *up)
{
    if (up == nullptr)
        return;
    free(up->body);
    *up = (Upstream){0};
}

static bool forward_common(const Provider *p, const char *path,
                           const char *json_body, bool stream,
                           provider_write_fn wcb, void *wud, long *status_out,
                           bool capture_headers,
                           char errbuf[CFG_ERRBUF_SIZE])
{
    if (p == nullptr || path == nullptr || json_body == nullptr ||
        wcb == nullptr) {
        set_err(errbuf, "provider_forward: null argument");
        return false;
    }

    char url[CFG_STR_URL + CFG_STR_ENDPOINT + 2u];
    snprintf(url, sizeof url, "%s%s", p->base_url, path);

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        set_err(errbuf, "curl_easy_init failed");
        return false;
    }

    char auth[CFG_STR_KEY + 32u];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", p->api_key);

    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs,
                             stream ? "Accept: text/event-stream"
                                    : "Accept: application/json");
    hdrs = curl_slist_append(hdrs, "User-Agent: funcroute/0.1");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)strlen(json_body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, wud);
    if (capture_headers) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, wud);
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, p->timeout_secs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        set_err(errbuf, "upstream request to %s failed: %s", url,
                curl_easy_strerror(rc));
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        return false;
    }
    if (status_out != nullptr)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return true;
}

bool provider_forward_cb(const Provider *p, const char *path,
                         const char *json_body, bool stream,
                         provider_write_fn wcb, void *wud, long *status_out,
                         char errbuf[CFG_ERRBUF_SIZE])
{
    return forward_common(p, path, json_body, stream, wcb, wud, status_out,
                          false, errbuf);
}

bool provider_forward(const Provider *p, const char *path, const char *json_body,
                      bool stream, Upstream *up, char errbuf[CFG_ERRBUF_SIZE])
{
    if (up == nullptr) {
        set_err(errbuf, "provider_forward: null Upstream");
        return false;
    }
    *up = (Upstream){0};
    if (!forward_common(p, path, json_body, stream, collect_cb, up, &up->status,
                        true, errbuf)) {
        upstream_free(up);
        return false;
    }
    return true;
}
