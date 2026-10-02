// config.c - load config.json with jansson, resolve API keys from env
#include "config.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

static bool read_str(json_t *obj, const char *key, char *dst, size_t dstsz)
{
    json_t *v = json_object_get(obj, key);
    if (!json_is_string(v))
        return false;
    snprintf(dst, dstsz, "%s", json_string_value(v));
    return true;
}

static bool read_long(json_t *obj, const char *key, long dflt, long *out)
{
    json_t *v = json_object_get(obj, key);
    if (json_is_integer(v)) {
        *out = json_integer_value(v);
        return true;
    }
    if (json_is_real(v)) {
        *out = (long)json_real_value(v);
        return true;
    }
    *out = dflt;
    return json_is_null(v) || v == nullptr;
}

// Resolve one routing provider key: an empty value inherits `fallback`, and
// whichever name results must be a configured provider. Used for the
// attachment routes (media/file/audio), whose names may be omitted.
static bool resolve_provider(const Config *cfg, char *field, size_t cap,
                             const char *fallback, const char *key,
                             char errbuf[CFG_ERRBUF_SIZE])
{
    if (field[0] == '\0') {
        snprintf(field, cap, "%s", fallback);
        if (fallback[0] == '\0') {
            set_err(errbuf, "routing.%s is not set and has no fallback", key);
            return false;
        }
    }
    if (config_find(cfg, field) == nullptr) {
        set_err(errbuf, "routing.%s \"%s\" is not a configured provider",
                key, field);
        return false;
    }
    return true;
}

bool config_load(Config *cfg, const char *path, char errbuf[CFG_ERRBUF_SIZE])
{
    if (cfg == nullptr || path == nullptr)
        return false;

    *cfg = (Config){
        .server = {.host = "127.0.0.1", .port = 8080, .max_connections = 128u},
        .routing = {.image_content_type = "image_url",
                    .audio_content_type = "input_audio",
                    .file_content_type = "file",
                    .endpoint = "/v1/chat/completions"},
    };

    json_error_t jerr;
    json_t *root = json_load_file(path, 0, &jerr);
    if (root == nullptr) {
        set_err(errbuf, "%s: %s (line %d)", path, jerr.text, jerr.line);
        return false;
    }
    if (!json_is_object(root)) {
        set_err(errbuf, "%s: top level must be an object", path);
        json_decref(root);
        return false;
    }

    // ---- server block ----
    json_t *srv = json_object_get(root, "server");
    if (json_is_object(srv)) {
        if (json_is_string(json_object_get(srv, "host")))
            read_str(srv, "host", cfg->server.host, sizeof cfg->server.host);
        if (json_is_integer(json_object_get(srv, "port"))) {
            json_int_t p = json_integer_value(json_object_get(srv, "port"));
            if (p < 0 || p > 65535) {
                set_err(errbuf, "server.port out of range: %lld", (long long)p);
                json_decref(root);
                return false;
            }
            cfg->server.port = (uint16_t)p;
        }
        if (json_is_integer(json_object_get(srv, "max_connections"))) {
            json_int_t m = json_integer_value(json_object_get(srv, "max_connections"));
            cfg->server.max_connections = (unsigned)(m > 0 ? m : 1);
        }
    }

    // ---- providers block (object of name -> {type, base_url, api_key..., model}) ----
    json_t *provs = json_object_get(root, "providers");
    if (!json_is_object(provs)) {
        set_err(errbuf, "missing object field \"providers\"");
        json_decref(root);
        return false;
    }
    const char *pname = nullptr;
    json_t *pobj = nullptr;
    json_object_foreach(provs, pname, pobj)
    {
        if (cfg->provider_count >= CFG_MAX_PROVIDERS) {
            set_err(errbuf, "too many providers (max %u)", CFG_MAX_PROVIDERS);
            json_decref(root);
            return false;
        }
        Provider *p = &cfg->providers[cfg->provider_count];
        *p = (Provider){.timeout_secs = 120};

        snprintf(p->name, sizeof p->name, "%s", pname);
        read_str(pobj, "type", p->type, sizeof p->type);
        if (p->type[0] == '\0')
            snprintf(p->type, sizeof p->type, "%s", "openai");

        if (!read_str(pobj, "base_url", p->base_url, sizeof p->base_url) ||
            p->base_url[0] == '\0') {
            set_err(errbuf, "provider \"%s\": missing \"base_url\"", pname);
            json_decref(root);
            return false;
        }
        if (!read_str(pobj, "model", p->model, sizeof p->model) ||
            p->model[0] == '\0') {
            set_err(errbuf, "provider \"%s\": missing \"model\"", pname);
            json_decref(root);
            return false;
        }
        read_str(pobj, "reasoning", p->reasoning, sizeof p->reasoning);

        // API key: config "api_key" wins, else env var named by "api_key_env".
        char envname[64] = "";
        read_str(pobj, "api_key_env", envname, sizeof envname);
        if (!read_str(pobj, "api_key", p->api_key, sizeof p->api_key) &&
            envname[0] != '\0') {
            const char *env = getenv(envname);
            if (env != nullptr)
                snprintf(p->api_key, sizeof p->api_key, "%s", env);
        }
        if (p->api_key[0] == '\0') {
            set_err(errbuf,
                    "provider \"%s\": no api_key and env \"%s\" is unset",
                    pname, envname);
            json_decref(root);
            return false;
        }

        read_long(pobj, "timeout_secs", 120, &p->timeout_secs);
        if (p->timeout_secs <= 0)
            p->timeout_secs = 120;
        cfg->provider_count++;
    }
    if (cfg->provider_count == 0u) {
        set_err(errbuf, "no providers configured");
        json_decref(root);
        return false;
    }

    // ---- routing block ----
    json_t *rt = json_object_get(root, "routing");
    if (json_is_object(rt)) {
        if (!read_str(rt, "default_provider", cfg->routing.default_provider,
                      sizeof cfg->routing.default_provider)) {
            set_err(errbuf, "routing: missing \"default_provider\"");
            json_decref(root);
            return false;
        }
        if (!read_str(rt, "image_provider", cfg->routing.image_provider,
                      sizeof cfg->routing.image_provider)) {
            set_err(errbuf, "routing: missing \"image_provider\"");
            json_decref(root);
            return false;
        }
        read_str(rt, "media_provider", cfg->routing.media_provider,
                 sizeof cfg->routing.media_provider);
        read_str(rt, "audio_provider", cfg->routing.audio_provider,
                 sizeof cfg->routing.audio_provider);
        read_str(rt, "file_provider", cfg->routing.file_provider,
                 sizeof cfg->routing.file_provider);
        read_str(rt, "image_content_type", cfg->routing.image_content_type,
                 sizeof cfg->routing.image_content_type);
        read_str(rt, "audio_content_type", cfg->routing.audio_content_type,
                 sizeof cfg->routing.audio_content_type);
        read_str(rt, "file_content_type", cfg->routing.file_content_type,
                 sizeof cfg->routing.file_content_type);
        read_str(rt, "endpoint", cfg->routing.endpoint, sizeof cfg->routing.endpoint);
        read_str(rt, "ollama_model", cfg->routing.ollama_model,
                 sizeof cfg->routing.ollama_model);
        long budget = 0;
        if (read_long(rt, "max_request_bytes", 0, &budget) && budget > 0)
            cfg->routing.max_request_bytes = (size_t)budget;
        if (read_long(rt, "max_messages", 0, &budget) && budget > 0)
            cfg->routing.max_messages = (size_t)budget;
        cfg->routing.advertised_context_length = DEFAULT_ADVERTISED_CONTEXT;
        if (read_long(rt, "advertised_context_length",
                      (long)DEFAULT_ADVERTISED_CONTEXT, &budget) &&
            budget > 0)
            cfg->routing.advertised_context_length = (size_t)budget;
    } else {
        set_err(errbuf, "missing object field \"routing\"");
        json_decref(root);
        return false;
    }

    // ---- optional database block ----
    snprintf(cfg->db_path, sizeof cfg->db_path, "%s", "funcroute.db");
    json_t *db = json_object_get(root, "database");
    if (json_is_object(db)) {
        // "path": "" disables SQLite persistence (stdout logging still on).
        if (json_is_string(json_object_get(db, "path")))
            read_str(db, "path", cfg->db_path, sizeof cfg->db_path);
    }

    json_decref(root);

    // Validate that routing references existing providers.
    if (config_find(cfg, cfg->routing.default_provider) == nullptr) {
        set_err(errbuf, "routing.default_provider \"%s\" is not a configured provider",
                cfg->routing.default_provider);
        return false;
    }
    if (config_find(cfg, cfg->routing.image_provider) == nullptr) {
        set_err(errbuf, "routing.image_provider \"%s\" is not a configured provider",
                cfg->routing.image_provider);
        return false;
    }
    // Attachment providers. An empty key falls back to the more general one so
    // existing configs (image_provider + media_provider only) keep working:
    //   media_provider <- image_provider
    //   file_provider  <- media_provider
    //   audio_provider <- media_provider
    // Every resolved name must reference a configured provider.
    if (!resolve_provider(cfg, cfg->routing.media_provider,
                          sizeof cfg->routing.media_provider,
                          cfg->routing.image_provider, "media_provider", errbuf))
        return false;
    if (!resolve_provider(cfg, cfg->routing.file_provider,
                          sizeof cfg->routing.file_provider,
                          cfg->routing.media_provider, "file_provider", errbuf))
        return false;
    if (!resolve_provider(cfg, cfg->routing.audio_provider,
                          sizeof cfg->routing.audio_provider,
                          cfg->routing.media_provider, "audio_provider", errbuf))
        return false;
    if (cfg->routing.endpoint[0] != '/') {
        set_err(errbuf, "routing.endpoint must start with '/'");
        return false;
    }
    return true;
}

const Provider *config_find(const Config *cfg, const char *name)
{
    if (cfg == nullptr || name == nullptr)
        return nullptr;
    for (size_t i = 0u; i < cfg->provider_count; ++i) {
        if (strcmp(cfg->providers[i].name, name) == 0)
            return &cfg->providers[i];
    }
    return nullptr;
}
