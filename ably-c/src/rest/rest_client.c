/*
 * Copyright 2024 Ben Gamble
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rest_client.h"
#include "base64.h"
#include "cJSON.h"

#include "ably/ably_rest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------------- */

void ably_rest_options_init(ably_rest_options_t *opts)
{
    assert(opts != NULL);
    opts->rest_host        = "rest.ably.io";
    opts->port             = 443;
    opts->timeout_ms       = 10000;
    opts->tls_verify_peer  = 1;
    opts->encoding         = ABLY_ENCODING_JSON;
    opts->ca_cert_pem_path = NULL;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

ably_rest_client_t *ably_rest_client_create(const char                *api_key,
                                              const ably_rest_options_t *opts,
                                              const ably_allocator_t    *allocator)
{
    if (!api_key || *api_key == '\0') return NULL;

    ably_rest_options_t defaults;
    if (!opts) { ably_rest_options_init(&defaults); opts = &defaults; }

    ably_allocator_t a = allocator ? *allocator : ably_system_allocator();

    ably_rest_client_t *c = ably_mem_malloc(&a, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->alloc    = a;
    c->encoding = opts->encoding;

    /* Build "Authorization: Basic <base64(api_key)>" header. */
    size_t key_len  = strlen(api_key);
    size_t b64_len  = ably_base64_encode_len(key_len);
    char  *b64_key  = ably_mem_malloc(&a, b64_len);
    if (!b64_key) goto fail;
    ably_base64_encode(b64_key, b64_len, (const uint8_t *)api_key, key_len);

    /* The auth_header string passed to http_client is the full header line. */
    char auth_header[ABLY_HTTP_AUTH_HEADER_MAX];
    int  n = snprintf(auth_header, sizeof(auth_header),
                      "Authorization: Basic %s", b64_key);
    ably_mem_free(&a, b64_key);
    if (n < 0 || (size_t)n >= sizeof(auth_header)) goto fail;

    /* Create HTTP client. */
    ably_http_options_t hopts;
    hopts.host               = opts->rest_host;
    hopts.port               = opts->port;
    hopts.timeout_ms         = opts->timeout_ms;
    hopts.tls_verify_peer    = opts->tls_verify_peer;
    hopts.ca_cert_pem_path   = opts->ca_cert_pem_path;

    c->http = ably_http_client_create(&hopts, auth_header, &a, &c->log);
    if (!c->http) goto fail;

    /* Pre-allocate body encoding buffer. */
    c->body_buf = ably_mem_malloc(&a, ABLY_HTTP_REQUEST_BUF_SIZE);
    if (!c->body_buf) goto fail;

    return c;

fail:
    if (c->http)     ably_http_client_destroy(c->http);
    if (c->body_buf) ably_mem_free(&a, c->body_buf);
    ably_mem_free(&a, c);
    return NULL;
}

void ably_rest_client_set_log_cb(ably_rest_client_t *client,
                                  ably_log_cb cb, void *user_data)
{
    assert(client != NULL);
    client->log.cb        = cb;
    client->log.user_data = user_data;
}

void ably_rest_client_destroy(ably_rest_client_t *client)
{
    if (!client) return;
    ably_http_client_destroy(client->http);
    ably_mem_free(&client->alloc, client->body_buf);
    ably_mem_free(&client->alloc, client);
}

long ably_rest_last_http_status(const ably_rest_client_t *client)
{
    assert(client != NULL);
    return client->last_http_status;
}

/* ---------------------------------------------------------------------------
 * Server time
 * --------------------------------------------------------------------------- */

ably_error_t ably_rest_time(ably_rest_client_t *client, int64_t *time_ms)
{
    assert(client  != NULL);
    assert(time_ms != NULL);

    *time_ms = 0;

    const char *body   = NULL;
    size_t      body_len = 0;
    ably_error_t err = ably_http_get(client->http, "/time",
                                      &client->last_http_status,
                                      &body, &body_len);
    if (err != ABLY_OK) return err;

    if (client->last_http_status < 200 || client->last_http_status >= 300) {
        ABLY_LOG_E(&client->log, "REST /time returned HTTP %ld",
                   client->last_http_status);
        return ABLY_ERR_HTTP;
    }

    /* Response is a JSON array with one element: [<timestamp_ms>] */
    cJSON *root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
    if (!root) return ABLY_ERR_PROTOCOL;

    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) >= 1) {
        cJSON *ts = cJSON_GetArrayItem(root, 0);
        if (ts && cJSON_IsNumber(ts))
            *time_ms = (int64_t)ts->valuedouble;
    }

    cJSON_Delete(root);
    return (*time_ms != 0) ? ABLY_OK : ABLY_ERR_PROTOCOL;
}

/* ---------------------------------------------------------------------------
 * URL-encode a channel name for use in the request path.
 * Only characters that are unsafe in URL paths are encoded.
 * Writes to dst (size dst_len); returns number of bytes written (excl. NUL).
 * --------------------------------------------------------------------------- */
static size_t url_encode_channel(char *dst, size_t dst_len, const char *channel)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (const char *p = channel; *p && out + 4 < dst_len; p++) {
        unsigned char c = (unsigned char)*p;
        /* Unreserved characters (RFC 3986): A-Z a-z 0-9 - _ . ~ */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0x0F];
            dst[out++] = hex[c & 0x0F];
        }
    }
    if (out < dst_len) dst[out] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Build JSON body for a single message (into c->body_buf).
 * Returns body length, or 0 on error.
 * --------------------------------------------------------------------------- */
static size_t build_single_json_body(ably_rest_client_t *client,
                                      const char *name, const char *data)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg) return 0;
    if (name) cJSON_AddStringToObject(msg, "name", name);
    if (data) cJSON_AddStringToObject(msg, "data", data);

    char *serialised = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!serialised) return 0;

    size_t len = strlen(serialised);
    if (len >= ABLY_HTTP_REQUEST_BUF_SIZE) { cJSON_free(serialised); return 0; }

    memcpy(client->body_buf, serialised, len + 1);
    cJSON_free(serialised);
    return len;
}

/* ---------------------------------------------------------------------------
 * Build JSON body for a batch of messages (into c->body_buf).
 * --------------------------------------------------------------------------- */
static size_t build_batch_json_body(ably_rest_client_t        *client,
                                     const ably_rest_message_t *messages,
                                     size_t                     count)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return 0;

    for (size_t i = 0; i < count; i++) {
        cJSON *msg = cJSON_CreateObject();
        if (!msg) { cJSON_Delete(arr); return 0; }
        if (messages[i].name) cJSON_AddStringToObject(msg, "name", messages[i].name);
        if (messages[i].data) cJSON_AddStringToObject(msg, "data", messages[i].data);
        cJSON_AddItemToArray(arr, msg);
    }

    char *serialised = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!serialised) return 0;

    size_t len = strlen(serialised);
    if (len >= ABLY_HTTP_REQUEST_BUF_SIZE) { cJSON_free(serialised); return 0; }

    memcpy(client->body_buf, serialised, len + 1);
    cJSON_free(serialised);
    return len;
}

/* ---------------------------------------------------------------------------
 * Publish
 * --------------------------------------------------------------------------- */

ably_error_t ably_rest_publish(ably_rest_client_t *client,
                                const char         *channel,
                                const char         *name,
                                const char         *data)
{
    assert(client != NULL);
    assert(channel != NULL);

    /* Build path: /channels/<url-encoded-channel>/messages */
    char path[512];
    char encoded_channel[256];
    url_encode_channel(encoded_channel, sizeof(encoded_channel), channel);
    snprintf(path, sizeof(path), "/channels/%s/messages", encoded_channel);

    size_t body_len = build_single_json_body(client, name, data);
    if (body_len == 0) return ABLY_ERR_INTERNAL;

    ably_error_t err = ably_http_post(client->http,
                                       path,
                                       "application/json",
                                       (const uint8_t *)client->body_buf,
                                       body_len,
                                       &client->last_http_status);
    if (err != ABLY_OK) return err;

    /* 201 Created is the expected success status for message publish. */
    if (client->last_http_status < 200 || client->last_http_status >= 300) {
        ABLY_LOG_E(&client->log, "REST publish returned HTTP %ld",
                   client->last_http_status);
        return ABLY_ERR_HTTP;
    }
    return ABLY_OK;
}

ably_error_t ably_rest_publish_batch(ably_rest_client_t        *client,
                                      const char                *channel,
                                      const ably_rest_message_t *messages,
                                      size_t                     count)
{
    assert(client != NULL);
    assert(channel != NULL);
    assert(messages != NULL);
    assert(count >= 1);

    char path[512];
    char encoded_channel[256];
    url_encode_channel(encoded_channel, sizeof(encoded_channel), channel);
    snprintf(path, sizeof(path), "/channels/%s/messages", encoded_channel);

    size_t body_len = build_batch_json_body(client, messages, count);
    if (body_len == 0) return ABLY_ERR_INTERNAL;

    ably_error_t err = ably_http_post(client->http,
                                       path,
                                       "application/json",
                                       (const uint8_t *)client->body_buf,
                                       body_len,
                                       &client->last_http_status);
    if (err != ABLY_OK) return err;

    if (client->last_http_status < 200 || client->last_http_status >= 300) {
        ABLY_LOG_E(&client->log, "REST batch publish returned HTTP %ld",
                   client->last_http_status);
        return ABLY_ERR_HTTP;
    }
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Channel history
 * --------------------------------------------------------------------------- */

ably_error_t ably_rest_channel_history(ably_rest_client_t   *client,
                                        const char           *channel,
                                        int                   limit,
                                        const char           *direction,
                                        const char           *from_serial,
                                        ably_history_page_t **page_out)
{
    assert(client   != NULL);
    assert(channel  != NULL);
    assert(page_out != NULL);

    *page_out = NULL;

    char path[1024];
    int  n;

    /* If from_serial is a full path (set from page->next_cursor after a Link header),
     * use it directly instead of building the path from scratch. */
    if (from_serial && from_serial[0] == '/') {
        n = snprintf(path, sizeof(path), "%s", from_serial);
    } else {
        char encoded_channel[ABLY_MAX_CHANNEL_NAME_LEN * 3];
        url_encode_channel(encoded_channel, sizeof(encoded_channel), channel);

        n = snprintf(path, sizeof(path), "/channels/%s/messages", encoded_channel);
        if (n < 0 || (size_t)n >= sizeof(path)) return ABLY_ERR_INTERNAL;

        char sep = '?';
        if (limit > 0) {
            int r = snprintf(path + n, sizeof(path) - (size_t)n, "%climit=%d", sep, limit);
            if (r > 0) { n += r; sep = '&'; }
        }
        if (direction && direction[0]) {
            int r = snprintf(path + n, sizeof(path) - (size_t)n, "%cdirection=%s", sep, direction);
            if (r > 0) { n += r; sep = '&'; }
        }
        if (from_serial && from_serial[0]) {
            int r = snprintf(path + n, sizeof(path) - (size_t)n, "%cfromSerial=%s", sep, from_serial);
            if (r > 0) { n += r; sep = '&'; }
        }
        (void)sep;
    }

    const char *body   = NULL;
    size_t      body_len = 0;
    ably_error_t err = ably_http_get(client->http, path,
                                      &client->last_http_status,
                                      &body, &body_len);
    if (err != ABLY_OK) return err;

    if (client->last_http_status < 200 || client->last_http_status >= 300) {
        ABLY_LOG_E(&client->log, "REST channel history returned HTTP %ld",
                   client->last_http_status);
        return ABLY_ERR_HTTP;
    }

    /* Parse JSON array of messages. */
    cJSON *root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        ABLY_LOG_E(&client->log, "Failed to parse history response as JSON array");
        return ABLY_ERR_PROTOCOL;
    }

    int item_count = cJSON_GetArraySize(root);
    ably_history_page_t *page = calloc(1,
        sizeof(ably_history_page_t) +
        (size_t)(item_count > 0 ? item_count : 1) * sizeof(ably_message_t));
    if (!page) { cJSON_Delete(root); return ABLY_ERR_NOMEM; }

    page->items = (ably_message_t *)((char *)page + sizeof(ably_history_page_t));
    page->count = 0;
    page->next_cursor[0] = '\0';

    cJSON *item = NULL;
    int i = 0;
    cJSON_ArrayForEach(item, root) {
        if (i >= item_count) break;
        ably_message_t *m = &page->items[i++];
        memset(m, 0, sizeof(*m));
        /* Fields point into cJSON's storage — which we're about to delete.
         * We must copy strings into the items we allocate. */
        cJSON *id_j   = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *name_j = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *data_j = cJSON_GetObjectItemCaseSensitive(item, "data");
        cJSON *cid_j  = cJSON_GetObjectItemCaseSensitive(item, "clientId");
        cJSON *ts_j   = cJSON_GetObjectItemCaseSensitive(item, "timestamp");

        /* history message fields are stored inline after the page struct */
        (void)id_j;   /* id: we don't store it here (not in ably_message_t at this level) */
        (void)name_j;
        (void)data_j;
        (void)cid_j;
        m->name      = name_j && cJSON_IsString(name_j) ? name_j->valuestring : NULL;
        m->data      = data_j && cJSON_IsString(data_j) ? data_j->valuestring : NULL;
        m->client_id = cid_j  && cJSON_IsString(cid_j)  ? cid_j->valuestring  : NULL;
        m->id        = id_j   && cJSON_IsString(id_j)   ? id_j->valuestring   : NULL;
        m->timestamp = ts_j   && cJSON_IsNumber(ts_j)   ? (int64_t)ts_j->valuedouble : 0;
        page->count++;
    }

    /* Note: cJSON_Delete(root) would free the strings m->name etc. point to.
     * We leave root alive — ably_history_page_free() owns and frees it.
     * Store the cJSON root pointer right before the page header (ugly but
     * avoids a second allocation).  We prepend it in a wrapper struct. */
    cJSON_Delete(root);

    /* For simplicity: deep-copy strings into the page allocation.
     * We need a different allocation strategy to avoid the cJSON lifetime issue.
     * Use a string pool appended after the items array. */
    free(page);

    /* --- Revised approach: allocate with string pool ---------------------- */
    item_count = cJSON_GetArraySize(NULL); /* can't reparse — body pointer is
                                              internal to http_client and still valid */

    /* Re-parse while body pointer is still valid (no other HTTP call made). */
    root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return ABLY_ERR_PROTOCOL;
    }
    item_count = cJSON_GetArraySize(root);

    /* Compute string pool size needed. */
    size_t pool_size = 0;
    cJSON_ArrayForEach(item, root) {
        cJSON *f;
        if ((f = cJSON_GetObjectItemCaseSensitive(item, "id"))       && cJSON_IsString(f)) pool_size += strlen(f->valuestring) + 1;
        if ((f = cJSON_GetObjectItemCaseSensitive(item, "name"))     && cJSON_IsString(f)) pool_size += strlen(f->valuestring) + 1;
        if ((f = cJSON_GetObjectItemCaseSensitive(item, "data"))     && cJSON_IsString(f)) pool_size += strlen(f->valuestring) + 1;
        if ((f = cJSON_GetObjectItemCaseSensitive(item, "clientId")) && cJSON_IsString(f)) pool_size += strlen(f->valuestring) + 1;
    }

    size_t items_size = (size_t)(item_count > 0 ? item_count : 1) * sizeof(ably_message_t);
    page = calloc(1, sizeof(ably_history_page_t) + items_size + pool_size + 1);
    if (!page) { cJSON_Delete(root); return ABLY_ERR_NOMEM; }

    page->items = (ably_message_t *)((char *)page + sizeof(ably_history_page_t));
    char *pool  = (char *)page->items + items_size;
    char *pool_ptr = pool;
    page->count = 0;
    page->next_cursor[0] = '\0';

#define COPY_STR(dst_field, json_key) do { \
    cJSON *_f = cJSON_GetObjectItemCaseSensitive(item, json_key); \
    if (_f && cJSON_IsString(_f)) { \
        size_t _l = strlen(_f->valuestring); \
        memcpy(pool_ptr, _f->valuestring, _l + 1); \
        m->dst_field = pool_ptr; \
        pool_ptr += _l + 1; \
    } \
} while (0)

    i = 0;
    cJSON_ArrayForEach(item, root) {
        if (i >= item_count) break;
        ably_message_t *m = &page->items[i++];
        memset(m, 0, sizeof(*m));
        COPY_STR(id,        "id");
        COPY_STR(name,      "name");
        COPY_STR(data,      "data");
        COPY_STR(client_id, "clientId");
        cJSON *ts_j = cJSON_GetObjectItemCaseSensitive(item, "timestamp");
        m->timestamp = (ts_j && cJSON_IsNumber(ts_j)) ? (int64_t)ts_j->valuedouble : 0;
        page->count++;
    }
#undef COPY_STR

    /* Populate next_cursor from the Link: header if present.
     * Format: <https://rest.ably.io/channels/foo/messages?...>; rel="next"
     * Extract the path+query portion so callers can use it directly. */
    const char *link = ably_http_last_link_header(client->http);
    if (link && link[0]) {
        /* Look for rel="next" */
        const char *rel_next = strstr(link, "rel=\"next\"");
        if (!rel_next) rel_next = strstr(link, "rel=next");
        if (rel_next) {
            /* Find the URL inside < ... > before the rel= part. */
            const char *lt = strchr(link, '<');
            const char *gt = lt ? strchr(lt, '>') : NULL;
            if (lt && gt && gt < rel_next) {
                lt++;
                size_t url_len = (size_t)(gt - lt);
                /* Strip the scheme+host prefix: find the third '/' */
                const char *path_start = lt;
                if (url_len > 8 && strncmp(lt, "https://", 8) == 0) {
                    path_start = strchr(lt + 8, '/');
                    if (!path_start) path_start = lt;
                }
                size_t path_len = (size_t)(gt - path_start);
                if (path_len < sizeof(page->next_cursor) - 1) {
                    memcpy(page->next_cursor, path_start, path_len);
                    page->next_cursor[path_len] = '\0';
                }
            }
        }
    }

    cJSON_Delete(root);
    *page_out = page;
    return ABLY_OK;
}

void ably_history_page_free(ably_history_page_t *page)
{
    free(page);
}

/* ---------------------------------------------------------------------------
 * Channel status
 * --------------------------------------------------------------------------- */

ably_error_t ably_rest_channel_status(ably_rest_client_t    *client,
                                       const char            *channel,
                                       ably_channel_status_t *out)
{
    assert(client  != NULL);
    assert(channel != NULL);
    assert(out     != NULL);

    memset(out, 0, sizeof(*out));

    char encoded_channel[ABLY_MAX_CHANNEL_NAME_LEN * 3];
    url_encode_channel(encoded_channel, sizeof(encoded_channel), channel);

    char path[512];
    snprintf(path, sizeof(path), "/channels/%s", encoded_channel);

    const char *body   = NULL;
    size_t      body_len = 0;
    ably_error_t err = ably_http_get(client->http, path,
                                      &client->last_http_status,
                                      &body, &body_len);
    if (err != ABLY_OK) return err;

    if (client->last_http_status < 200 || client->last_http_status >= 300) {
        ABLY_LOG_E(&client->log, "REST channel status returned HTTP %ld",
                   client->last_http_status);
        return ABLY_ERR_HTTP;
    }

    cJSON *root = body ? cJSON_ParseWithLength(body, body_len) : NULL;
    if (!root) return ABLY_ERR_PROTOCOL;

    cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (name_j && cJSON_IsString(name_j)) {
        strncpy(out->name, name_j->valuestring, ABLY_MAX_CHANNEL_NAME_LEN - 1);
        out->name[ABLY_MAX_CHANNEL_NAME_LEN - 1] = '\0';
    }

    cJSON *status_j = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (status_j && cJSON_IsObject(status_j)) {
        cJSON *active_j = cJSON_GetObjectItemCaseSensitive(status_j, "isActive");
        out->is_active = (active_j && cJSON_IsTrue(active_j)) ? 1 : 0;

        cJSON *occ_j = cJSON_GetObjectItemCaseSensitive(status_j, "occupancy");
        if (occ_j && cJSON_IsObject(occ_j)) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(occ_j, "metrics");
            if (m && cJSON_IsObject(m)) {
#define GET_INT(field, key) do { \
    cJSON *_f = cJSON_GetObjectItemCaseSensitive(m, key); \
    out->occupancy.field = (_f && cJSON_IsNumber(_f)) ? (int)_f->valuedouble : 0; \
} while (0)
                GET_INT(connections,          "connections");
                GET_INT(publishers,           "publishers");
                GET_INT(subscribers,          "subscribers");
                GET_INT(presence_connections, "presenceConnections");
                GET_INT(presence_members,     "presenceMembers");
                GET_INT(presence_subscribers, "presenceSubscribers");
#undef GET_INT
            }
        }
    }

    cJSON_Delete(root);
    return ABLY_OK;
}
