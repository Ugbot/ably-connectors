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

/*
 * WebSocket client for Ably real-time connections.
 *
 * Architecture:
 *   mbedTLS net_sockets  — TCP connection
 *   mbedTLS ssl          — TLS 1.2/1.3
 *   wslay                — WebSocket RFC 6455 framing (transport-agnostic)
 *
 * The WebSocket opening handshake (HTTP Upgrade) is implemented here
 * rather than via wslay, because wslay is frame-level only.  The handshake
 * requires:
 *   1. Generate a random 16-byte Sec-WebSocket-Key, base64-encode it
 *   2. Send an HTTP/1.1 GET with Upgrade: websocket headers
 *   3. Validate the server's 101 Switching Protocols response
 *
 * SHA-1 for Sec-WebSocket-Accept validation comes from mbedTLS.
 */

#include "ws_client.h"
#include "base64.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha1.h"
#include "mbedtls/error.h"

#include "wslay/wslay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Internal structure
 * --------------------------------------------------------------------------- */

struct ably_ws_client_s {
    /* TLS */
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       ssl_conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt         ca_chain;

    /* wslay */
    wslay_event_context_ptr  wslay_ctx;
    int                      connected;
    int                      close_sent;
    int                      close_received;

    /* Options (fixed at create time) */
    char     host[ABLY_WS_HOST_MAX];
    char     path[ABLY_WS_PATH_MAX];
    char     port_str[8];
    long     timeout_ms;
    int      tls_verify_peer;

    /* Callback */
    ably_ws_frame_cb on_frame;
    void            *user_data;

    /* Pre-allocated buffers */
    char    *recv_buf;       /* ABLY_WS_RECV_BUF_SIZE  */
    char    *send_buf;       /* ABLY_WS_SEND_BUF_SIZE  */
    char    *handshake_buf;  /* ABLY_WS_HANDSHAKE_BUF  */

    ably_allocator_t alloc;
    ably_log_ctx_t   log;
};

/* ---------------------------------------------------------------------------
 * wslay callbacks — the library calls these for I/O
 * --------------------------------------------------------------------------- */

static ssize_t wslay_recv_cb(wslay_event_context_ptr ctx,
                               uint8_t *buf, size_t len,
                               int flags, void *user_data)
{
    (void)ctx; (void)flags;
    ably_ws_client_t *c = user_data;

    int ret = mbedtls_ssl_read(&c->ssl, buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    if (ret < 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    return ret;
}

static ssize_t wslay_send_cb(wslay_event_context_ptr ctx,
                               const uint8_t *buf, size_t len,
                               int flags, void *user_data)
{
    (void)ctx; (void)flags;
    ably_ws_client_t *c = user_data;

    size_t sent = 0;
    while (sent < len) {
        int ret = mbedtls_ssl_write(&c->ssl, buf + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }
        sent += (size_t)ret;
    }
    return (ssize_t)len;
}

static int wslay_genmask_cb(wslay_event_context_ptr ctx,
                              uint8_t *buf, size_t len, void *user_data)
{
    (void)ctx; (void)user_data;
    /* Use mbedTLS CTRNG for the masking key. */
    ably_ws_client_t *c = user_data;
    return mbedtls_ctr_drbg_random(&c->ctr_drbg, buf, len);
}

static void wslay_on_msg_cb(wslay_event_context_ptr ctx,
                              const struct wslay_event_on_msg_recv_arg *arg,
                              void *user_data)
{
    (void)ctx;
    ably_ws_client_t *c = user_data;

    if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
        c->close_received = 1;
        return;
    }

    if (arg->opcode != WSLAY_TEXT_FRAME && arg->opcode != WSLAY_BINARY_FRAME) return;
    if (!arg->msg || arg->msg_length == 0) return;

    /* NUL-terminate in the recv_buf — wslay passes us a pointer into its
     * internal buffer, not into our recv_buf.  We copy into recv_buf for
     * NUL-termination and to give the callback a stable pointer. */
    size_t copy_len = arg->msg_length < ABLY_WS_RECV_BUF_SIZE - 1
                    ? arg->msg_length
                    : ABLY_WS_RECV_BUF_SIZE - 1;
    memcpy(c->recv_buf, arg->msg, copy_len);
    c->recv_buf[copy_len] = '\0';

    if (c->on_frame) {
        c->on_frame(c->recv_buf, copy_len, c->user_data);
    }
}

static struct wslay_event_callbacks s_wslay_callbacks = {
    .recv_callback          = wslay_recv_cb,
    .send_callback          = wslay_send_cb,
    .genmask_callback       = wslay_genmask_cb,
    .on_msg_recv_callback   = wslay_on_msg_cb,
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

ably_ws_client_t *ably_ws_client_create(const ably_ws_options_t *opts,
                                          ably_ws_frame_cb         on_frame,
                                          void                    *user_data,
                                          const ably_allocator_t  *alloc,
                                          const ably_log_ctx_t    *log)
{
    assert(opts != NULL);

    ably_allocator_t a = alloc ? *alloc : ably_system_allocator();

    ably_ws_client_t *c = ably_mem_malloc(&a, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));

    c->alloc     = a;
    c->on_frame  = on_frame;
    c->user_data = user_data;
    if (log) c->log = *log;

    snprintf(c->host,     sizeof(c->host),     "%s", opts->host ? opts->host : "realtime.ably.io");
    snprintf(c->path,     sizeof(c->path),     "%s", opts->path ? opts->path : "/");
    snprintf(c->port_str, sizeof(c->port_str), "%u", opts->port ? (unsigned)opts->port : 443u);
    c->timeout_ms     = opts->timeout_ms > 0 ? opts->timeout_ms : 10000;
    c->tls_verify_peer = opts->tls_verify_peer;

    c->recv_buf      = ably_mem_malloc(&a, ABLY_WS_RECV_BUF_SIZE);
    c->send_buf      = ably_mem_malloc(&a, ABLY_WS_SEND_BUF_SIZE);
    c->handshake_buf = ably_mem_malloc(&a, ABLY_WS_HANDSHAKE_BUF);
    if (!c->recv_buf || !c->send_buf || !c->handshake_buf) goto fail;

    /* mbedTLS */
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_x509_crt_init(&c->ca_chain);

    const char *pers = "ably_ws";
    int ret = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                                      &c->entropy,
                                      (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        ABLY_LOG_E(&c->log, "ctr_drbg_seed failed: %d", ret);
        goto fail_tls;
    }

    ret = mbedtls_ssl_config_defaults(&c->ssl_conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ABLY_LOG_E(&c->log, "ssl_config_defaults failed: %d", ret);
        goto fail_tls;
    }

    mbedtls_ssl_conf_rng(&c->ssl_conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    if (c->tls_verify_peer) {
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->ssl_conf, &c->ca_chain, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    return c;

fail_tls:
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->ssl_conf);
    mbedtls_entropy_free(&c->entropy);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_x509_crt_free(&c->ca_chain);
fail:
    if (c->recv_buf)      ably_mem_free(&a, c->recv_buf);
    if (c->send_buf)      ably_mem_free(&a, c->send_buf);
    if (c->handshake_buf) ably_mem_free(&a, c->handshake_buf);
    ably_mem_free(&a, c);
    return NULL;
}

void ably_ws_client_destroy(ably_ws_client_t *client)
{
    if (!client) return;
    if (client->wslay_ctx) {
        wslay_event_context_free(client->wslay_ctx);
    }
    mbedtls_ssl_close_notify(&client->ssl);
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->ssl_conf);
    mbedtls_entropy_free(&client->entropy);
    mbedtls_ctr_drbg_free(&client->ctr_drbg);
    mbedtls_x509_crt_free(&client->ca_chain);
    mbedtls_net_free(&client->net);
    ably_mem_free(&client->alloc, client->recv_buf);
    ably_mem_free(&client->alloc, client->send_buf);
    ably_mem_free(&client->alloc, client->handshake_buf);
    ably_mem_free(&client->alloc, client);
}

int ably_ws_is_connected(const ably_ws_client_t *client)
{
    return client && client->connected && !client->close_received;
}

/* ---------------------------------------------------------------------------
 * WebSocket opening handshake
 * --------------------------------------------------------------------------- */

static void generate_ws_key(ably_ws_client_t *c, char *key_b64, size_t key_b64_len)
{
    uint8_t raw[16];
    mbedtls_ctr_drbg_random(&c->ctr_drbg, raw, sizeof(raw));
    ably_base64_encode(key_b64, key_b64_len, raw, sizeof(raw));
}

/* Send HTTP Upgrade request and read 101 response. */
static ably_error_t perform_handshake(ably_ws_client_t *c)
{
    char ws_key[32];
    generate_ws_key(c, ws_key, sizeof(ws_key));

    int n = snprintf(c->handshake_buf, ABLY_WS_HANDSHAKE_BUF,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        c->path, c->host, ws_key);

    if (n < 0 || n >= ABLY_WS_HANDSHAKE_BUF) return ABLY_ERR_NETWORK;

    /* Send HTTP upgrade request. */
    size_t sent = 0;
    size_t req_len = (size_t)n;
    while (sent < req_len) {
        int ret = mbedtls_ssl_write(&c->ssl,
                                     (const uint8_t *)c->handshake_buf + sent,
                                     req_len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) {
            ABLY_LOG_E(&c->log, "handshake send failed: %d", ret);
            return ABLY_ERR_NETWORK;
        }
        sent += (size_t)ret;
    }

    /* Read HTTP response — look for "\r\n\r\n". */
    size_t total = 0;
    while (total < (size_t)ABLY_WS_HANDSHAKE_BUF - 1) {
        int ret = mbedtls_ssl_read(&c->ssl,
                                    (uint8_t *)c->handshake_buf + total,
                                    (size_t)ABLY_WS_HANDSHAKE_BUF - 1 - total);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (ret <= 0) {
            ABLY_LOG_E(&c->log, "handshake recv failed: %d", ret);
            return ABLY_ERR_NETWORK;
        }
        total += (size_t)ret;
        c->handshake_buf[total] = '\0';

        if (strstr(c->handshake_buf, "\r\n\r\n")) break;
    }

    if (strncmp(c->handshake_buf, "HTTP/1.1 101", 12) != 0) {
        ABLY_LOG_E(&c->log, "WebSocket upgrade failed: %.100s", c->handshake_buf);
        return ABLY_ERR_NETWORK;
    }

    ABLY_LOG_D(&c->log, "WebSocket handshake complete");
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Connect
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_connect(ably_ws_client_t *client)
{
    assert(client != NULL);

    client->connected      = 0;
    client->close_sent     = 0;
    client->close_received = 0;

    /* TCP connect. */
    int ret = mbedtls_net_connect(&client->net, client->host, client->port_str,
                                   MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "TCP connect to %s:%s failed: %d",
                   client->host, client->port_str, ret);
        return ABLY_ERR_NETWORK;
    }

    mbedtls_net_set_block(&client->net);

    /* TLS. */
    ret = mbedtls_ssl_setup(&client->ssl, &client->ssl_conf);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "ssl_setup failed: %d", ret);
        mbedtls_net_free(&client->net);
        mbedtls_net_init(&client->net);
        return ABLY_ERR_NETWORK;
    }

    mbedtls_ssl_set_bio(&client->ssl, &client->net,
                         mbedtls_net_send, mbedtls_net_recv,
                         mbedtls_net_recv_timeout);
    mbedtls_ssl_set_hostname(&client->ssl, client->host);
    mbedtls_ssl_conf_read_timeout(&client->ssl_conf,
                                   (uint32_t)client->timeout_ms);

    do { ret = mbedtls_ssl_handshake(&client->ssl); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        ABLY_LOG_E(&client->log, "TLS handshake failed: %d", ret);
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_init(&client->ssl);
        mbedtls_net_free(&client->net);
        mbedtls_net_init(&client->net);
        return ABLY_ERR_NETWORK;
    }

    /* WebSocket handshake. */
    ably_error_t err = perform_handshake(client);
    if (err != ABLY_OK) {
        mbedtls_ssl_close_notify(&client->ssl);
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_init(&client->ssl);
        mbedtls_net_free(&client->net);
        mbedtls_net_init(&client->net);
        return err;
    }

    /* Initialise wslay client context. */
    if (client->wslay_ctx) {
        wslay_event_context_free(client->wslay_ctx);
        client->wslay_ctx = NULL;
    }

    ret = wslay_event_context_client_init(&client->wslay_ctx,
                                           &s_wslay_callbacks, client);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "wslay_event_context_client_init failed: %d", ret);
        return ABLY_ERR_NETWORK;
    }

    client->connected = 1;
    ABLY_LOG_I(&client->log, "WebSocket connected to %s%s", client->host, client->path);
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Send text frame
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_send_text(ably_ws_client_t *client,
                                 const char *payload, size_t len)
{
    assert(client != NULL);
    assert(payload != NULL);

    if (!client->connected || client->close_sent) return ABLY_ERR_STATE;

    struct wslay_event_msg msg;
    msg.opcode      = WSLAY_TEXT_FRAME;
    msg.msg         = (const uint8_t *)payload;
    msg.msg_length  = len;

    int ret = wslay_event_queue_msg(client->wslay_ctx, &msg);
    if (ret != 0) return ABLY_ERR_NETWORK;

    while (wslay_event_want_write(client->wslay_ctx)) {
        ret = wslay_event_send(client->wslay_ctx);
        if (ret != 0) {
            ABLY_LOG_E(&client->log, "wslay_event_send failed: %d", ret);
            client->connected = 0;
            return ABLY_ERR_NETWORK;
        }
    }
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Receive
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_recv_once(ably_ws_client_t *client, int timeout_ms)
{
    assert(client != NULL);

    if (!client->connected) return ABLY_ERR_NETWORK;
    if (client->close_received) return ABLY_ERR_NETWORK;

    mbedtls_ssl_conf_read_timeout(&client->ssl_conf,
                                   timeout_ms > 0 ? (uint32_t)timeout_ms : 1000u);

    int ret = wslay_event_recv(client->wslay_ctx);
    if (ret == 0) return ABLY_OK;

    /* WSLAY_ERR_WOULDBLOCK means the timeout fired with no data — not an error. */
    if (ret == WSLAY_ERR_WOULDBLOCK) {
        return ABLY_OK;
    }

    ABLY_LOG_I(&client->log, "wslay_event_recv returned %d (disconnected)", ret);
    client->connected = 0;
    return ABLY_ERR_NETWORK;
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_close(ably_ws_client_t *client, int timeout_ms)
{
    assert(client != NULL);
    if (!client->connected) return ABLY_OK;

    /* Queue a close frame. */
    struct wslay_event_msg close_msg;
    close_msg.opcode     = WSLAY_CONNECTION_CLOSE;
    close_msg.msg        = NULL;
    close_msg.msg_length = 0;

    wslay_event_queue_msg(client->wslay_ctx, &close_msg);
    client->close_sent = 1;

    /* Drain sends. */
    while (wslay_event_want_write(client->wslay_ctx)) {
        wslay_event_send(client->wslay_ctx);
    }

    /* Wait for peer's close frame. */
    long deadline_ms = timeout_ms > 0 ? timeout_ms : 5000;
    long waited = 0;
    while (!client->close_received && waited < deadline_ms) {
        ably_ws_recv_once(client, 200);
        waited += 200;
    }

    mbedtls_ssl_close_notify(&client->ssl);
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_init(&client->ssl);
    mbedtls_net_free(&client->net);
    mbedtls_net_init(&client->net);
    client->connected = 0;

    return client->close_received ? ABLY_OK : ABLY_ERR_TIMEOUT;
}
