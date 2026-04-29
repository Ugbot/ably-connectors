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
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------------- */

void ably_rest_options_init(ably_rest_options_t *opts)
{
    assert(opts != NULL);
    opts->rest_host      = "rest.ably.io";
    opts->port           = 443;
    opts->timeout_ms     = 10000;
    opts->tls_verify_peer = 1;
    opts->encoding       = ABLY_ENCODING_JSON;
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
    hopts.host            = opts->rest_host;
    hopts.port            = opts->port;
    hopts.timeout_ms      = opts->timeout_ms;
    hopts.tls_verify_peer = opts->tls_verify_peer;

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
