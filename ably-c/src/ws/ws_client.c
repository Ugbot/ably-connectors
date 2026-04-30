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
 * WebSocket transport for Ably real-time connections.
 *
 * Architecture:
 *   mbedTLS net_sockets  — TCP connection (non-blocking)
 *   mbedTLS ssl          — TLS 1.2/1.3
 *   wslay                — WebSocket RFC 6455 framing (transport-agnostic)
 *
 * I/O model (uWebSockets-inspired):
 *   Sockets run in non-blocking mode.  All timeouts are enforced by our own
 *   poll() wrappers rather than mbedTLS's internal select()-based timeout
 *   mechanism.  This avoids mutating the shared ssl_conf object (which is
 *   not safe to do per-connection) and eliminates the race between TLS 1.3
 *   NewSessionTicket messages and the HTTP upgrade read on CloudFront.
 *
 * WebSocket opening handshake:
 *   1. Generate a random 16-byte Sec-WebSocket-Key, base64-encode it.
 *   2. Send an HTTP/1.1 GET with Upgrade: websocket headers.
 *   3. Validate the server's 101 Switching Protocols response.
 *   SHA-1 for Sec-WebSocket-Accept validation comes from mbedTLS.
 */

#include "ws_client.h"
#include "base64.h"
#include "tls_ca.h"

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

#ifdef _WIN32
#  include <winsock2.h>
#  define poll WSAPoll
#else
#  include <poll.h>
#endif

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
 * Non-blocking I/O helpers (uWebSockets pattern)
 *
 * These are the only places that enforce timeouts.  The ssl_conf object is
 * never touched per-connection — it is set once at create time and then left
 * alone.  This eliminates the shared-state race that caused -80 resets.
 * --------------------------------------------------------------------------- */

static int tls_poll_for_read(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    return poll(&pfd, 1, timeout_ms);
}

static int tls_poll_for_write(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    return poll(&pfd, 1, timeout_ms);
}

/* ---------------------------------------------------------------------------
 * wslay callbacks — the library calls these for I/O
 * --------------------------------------------------------------------------- */

static ssize_t wslay_recv_callback(wslay_event_context_ptr ctx,
                                    uint8_t *buffer, size_t buffer_length,
                                    int flags, void *user_data)
{
    (void)ctx; (void)flags;
    ably_ws_client_t *transport = user_data;

    int tls_result;
    do {
        tls_result = mbedtls_ssl_read(&transport->ssl, buffer, buffer_length);
    } while (tls_result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET);

    if (tls_result == MBEDTLS_ERR_SSL_WANT_READ) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    if (tls_result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || tls_result == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    if (tls_result < 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    return tls_result;
}

static ssize_t wslay_send_callback(wslay_event_context_ptr ctx,
                                    const uint8_t *buffer, size_t buffer_length,
                                    int flags, void *user_data)
{
    (void)ctx; (void)flags;
    ably_ws_client_t *transport = user_data;

    size_t total_bytes_sent = 0;
    while (total_bytes_sent < buffer_length) {
        int tls_result = mbedtls_ssl_write(&transport->ssl,
                                            buffer + total_bytes_sent,
                                            buffer_length - total_bytes_sent);
        if (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_poll_for_write(transport->net.fd, (int)transport->timeout_ms);
            continue;
        }
        if (tls_result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (tls_result <= 0) {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }
        total_bytes_sent += (size_t)tls_result;
    }
    return (ssize_t)buffer_length;
}

static int wslay_genmask_callback(wslay_event_context_ptr ctx,
                                   uint8_t *buffer, size_t buffer_length,
                                   void *user_data)
{
    (void)ctx;
    ably_ws_client_t *transport = user_data;
    return mbedtls_ctr_drbg_random(&transport->ctr_drbg, buffer, buffer_length);
}

static void wslay_on_message_received(wslay_event_context_ptr ctx,
                                       const struct wslay_event_on_msg_recv_arg *arg,
                                       void *user_data)
{
    (void)ctx;
    ably_ws_client_t *transport = user_data;

    if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
        transport->close_received = 1;
        return;
    }

    if (arg->opcode != WSLAY_TEXT_FRAME && arg->opcode != WSLAY_BINARY_FRAME) return;
    if (!arg->msg || arg->msg_length == 0) return;

    /* NUL-terminate in the recv_buf — wslay passes us a pointer into its
     * internal buffer, not into our recv_buf.  We copy into recv_buf for
     * NUL-termination and to give the callback a stable pointer. */
    size_t copy_length = arg->msg_length < ABLY_WS_RECV_BUF_SIZE - 1
                       ? arg->msg_length
                       : ABLY_WS_RECV_BUF_SIZE - 1;
    memcpy(transport->recv_buf, arg->msg, copy_length);
    transport->recv_buf[copy_length] = '\0';

    if (transport->on_frame) {
        transport->on_frame(transport->recv_buf, copy_length, transport->user_data);
    }
}

static struct wslay_event_callbacks wslay_callbacks = {
    .recv_callback          = wslay_recv_callback,
    .send_callback          = wslay_send_callback,
    .genmask_callback       = wslay_genmask_callback,
    .on_msg_recv_callback   = wslay_on_message_received,
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

    ably_allocator_t allocator = alloc ? *alloc : ably_system_allocator();

    ably_ws_client_t *transport = ably_mem_malloc(&allocator, sizeof(*transport));
    if (!transport) return NULL;
    memset(transport, 0, sizeof(*transport));

    transport->alloc     = allocator;
    transport->on_frame  = on_frame;
    transport->user_data = user_data;
    if (log) transport->log = *log;

    snprintf(transport->host,     sizeof(transport->host),     "%s", opts->host ? opts->host : "realtime.ably.io");
    snprintf(transport->path,     sizeof(transport->path),     "%s", opts->path ? opts->path : "/");
    snprintf(transport->port_str, sizeof(transport->port_str), "%u", opts->port ? (unsigned)opts->port : 443u);
    transport->timeout_ms      = opts->timeout_ms > 0 ? opts->timeout_ms : 10000;
    transport->tls_verify_peer = opts->tls_verify_peer;

    transport->recv_buf      = ably_mem_malloc(&allocator, ABLY_WS_RECV_BUF_SIZE);
    transport->send_buf      = ably_mem_malloc(&allocator, ABLY_WS_SEND_BUF_SIZE);
    transport->handshake_buf = ably_mem_malloc(&allocator, ABLY_WS_HANDSHAKE_BUF);
    if (!transport->recv_buf || !transport->send_buf || !transport->handshake_buf) goto fail;

    /* mbedTLS */
    mbedtls_net_init(&transport->net);
    mbedtls_ssl_init(&transport->ssl);
    mbedtls_ssl_config_init(&transport->ssl_conf);
    mbedtls_entropy_init(&transport->entropy);
    mbedtls_ctr_drbg_init(&transport->ctr_drbg);
    mbedtls_x509_crt_init(&transport->ca_chain);

    const char *drbg_personalization = "ably_ws";
    int tls_result = mbedtls_ctr_drbg_seed(&transport->ctr_drbg,
                                             mbedtls_entropy_func,
                                             &transport->entropy,
                                             (const unsigned char *)drbg_personalization,
                                             strlen(drbg_personalization));
    if (tls_result != 0) {
        ABLY_LOG_E(&transport->log, "ctr_drbg_seed failed: %d", tls_result);
        goto fail_tls;
    }

    tls_result = mbedtls_ssl_config_defaults(&transport->ssl_conf,
                                              MBEDTLS_SSL_IS_CLIENT,
                                              MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
    if (tls_result != 0) {
        ABLY_LOG_E(&transport->log, "ssl_config_defaults failed: %d", tls_result);
        goto fail_tls;
    }

    mbedtls_ssl_conf_rng(&transport->ssl_conf,
                          mbedtls_ctr_drbg_random, &transport->ctr_drbg);

    if (transport->tls_verify_peer) {
        ably_tls_load_system_ca(&transport->ca_chain, &transport->log);
        mbedtls_ssl_conf_authmode(&transport->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&transport->ssl_conf, &transport->ca_chain, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&transport->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    return transport;

fail_tls:
    mbedtls_ssl_free(&transport->ssl);
    mbedtls_ssl_config_free(&transport->ssl_conf);
    mbedtls_entropy_free(&transport->entropy);
    mbedtls_ctr_drbg_free(&transport->ctr_drbg);
    mbedtls_x509_crt_free(&transport->ca_chain);
fail:
    if (transport->recv_buf)      ably_mem_free(&allocator, transport->recv_buf);
    if (transport->send_buf)      ably_mem_free(&allocator, transport->send_buf);
    if (transport->handshake_buf) ably_mem_free(&allocator, transport->handshake_buf);
    ably_mem_free(&allocator, transport);
    return NULL;
}

void ably_ws_client_destroy(ably_ws_client_t *transport)
{
    if (!transport) return;
    if (transport->wslay_ctx) {
        wslay_event_context_free(transport->wslay_ctx);
    }
    mbedtls_ssl_close_notify(&transport->ssl);
    mbedtls_ssl_free(&transport->ssl);
    mbedtls_ssl_config_free(&transport->ssl_conf);
    mbedtls_entropy_free(&transport->entropy);
    mbedtls_ctr_drbg_free(&transport->ctr_drbg);
    mbedtls_x509_crt_free(&transport->ca_chain);
    mbedtls_net_free(&transport->net);
    ably_mem_free(&transport->alloc, transport->recv_buf);
    ably_mem_free(&transport->alloc, transport->send_buf);
    ably_mem_free(&transport->alloc, transport->handshake_buf);
    ably_mem_free(&transport->alloc, transport);
}

int ably_ws_is_connected(const ably_ws_client_t *transport)
{
    return transport && transport->connected && !transport->close_received;
}

int ably_ws_client_fd(const ably_ws_client_t *transport)
{
    if (!transport || !transport->connected) return -1;
    return (int)transport->net.fd;
}

/* ---------------------------------------------------------------------------
 * WebSocket opening handshake (HTTP Upgrade)
 * --------------------------------------------------------------------------- */

static void generate_websocket_key(ably_ws_client_t *transport,
                                    char *key_b64_out, size_t key_b64_capacity)
{
    uint8_t raw_key_bytes[16];
    mbedtls_ctr_drbg_random(&transport->ctr_drbg, raw_key_bytes, sizeof(raw_key_bytes));
    ably_base64_encode(key_b64_out, key_b64_capacity, raw_key_bytes, sizeof(raw_key_bytes));
}

static ably_error_t perform_websocket_upgrade_handshake(ably_ws_client_t *transport)
{
    char websocket_key[32];
    generate_websocket_key(transport, websocket_key, sizeof(websocket_key));

    int request_length = snprintf(transport->handshake_buf, ABLY_WS_HANDSHAKE_BUF,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        transport->path, transport->host, websocket_key);

    if (request_length < 0 || request_length >= ABLY_WS_HANDSHAKE_BUF) {
        return ABLY_ERR_NETWORK;
    }

    /* Send HTTP upgrade request.
     *
     * With non-blocking I/O, ssl_write may return WANT_WRITE if the kernel
     * send buffer is full — we poll until writable and retry.  TLS 1.3 may
     * also surface RECEIVED_NEW_SESSION_TICKET during write — retry immediately
     * as the data is already buffered in mbedTLS. */
    size_t total_bytes_sent = 0;
    while (total_bytes_sent < (size_t)request_length) {
        int tls_result = mbedtls_ssl_write(&transport->ssl,
                                            (const uint8_t *)transport->handshake_buf + total_bytes_sent,
                                            (size_t)request_length - total_bytes_sent);
        if (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int poll_result = tls_poll_for_write(transport->net.fd, (int)transport->timeout_ms);
            if (poll_result == 0) {
                ABLY_LOG_E(&transport->log, "handshake send timed out");
                return ABLY_ERR_TIMEOUT;
            }
            if (poll_result < 0) {
                ABLY_LOG_E(&transport->log, "handshake send poll error");
                return ABLY_ERR_NETWORK;
            }
            continue;
        }
        if (tls_result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (tls_result <= 0) {
            ABLY_LOG_E(&transport->log, "handshake send failed: %d", tls_result);
            return ABLY_ERR_NETWORK;
        }
        total_bytes_sent += (size_t)tls_result;
    }

    /* Read HTTP 101 response.
     *
     * With non-blocking I/O, ssl_read returns WANT_READ when the kernel has
     * no data — we poll until readable and retry.  TLS 1.3 NewSessionTicket
     * messages are surfaced as RECEIVED_NEW_SESSION_TICKET; they carry no
     * application data so we discard them and retry immediately. */
    size_t total_bytes_received = 0;
    while (total_bytes_received < (size_t)ABLY_WS_HANDSHAKE_BUF - 1) {
        int tls_result = mbedtls_ssl_read(&transport->ssl,
                                           (uint8_t *)transport->handshake_buf + total_bytes_received,
                                           (size_t)ABLY_WS_HANDSHAKE_BUF - 1 - total_bytes_received);
        if (tls_result == MBEDTLS_ERR_SSL_WANT_READ) {
            int poll_result = tls_poll_for_read(transport->net.fd, (int)transport->timeout_ms);
            if (poll_result == 0) {
                ABLY_LOG_E(&transport->log,
                           "handshake recv timed out after %zu bytes",
                           total_bytes_received);
                return ABLY_ERR_TIMEOUT;
            }
            if (poll_result < 0) {
                ABLY_LOG_E(&transport->log, "handshake recv poll error");
                return ABLY_ERR_NETWORK;
            }
            continue;
        }
        if (tls_result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (tls_result <= 0) {
            ABLY_LOG_E(&transport->log,
                       "handshake recv failed: %d (received %zu bytes so far: %.200s)",
                       tls_result,
                       total_bytes_received,
                       total_bytes_received > 0 ? transport->handshake_buf : "(none)");
            return ABLY_ERR_NETWORK;
        }
        total_bytes_received += (size_t)tls_result;
        transport->handshake_buf[total_bytes_received] = '\0';

        if (strstr(transport->handshake_buf, "\r\n\r\n")) break;
    }

    if (strncmp(transport->handshake_buf, "HTTP/1.1 101", 12) != 0) {
        ABLY_LOG_E(&transport->log, "WebSocket upgrade rejected: %.100s",
                   transport->handshake_buf);
        return ABLY_ERR_NETWORK;
    }

    ABLY_LOG_D(&transport->log, "WebSocket upgrade handshake complete");
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Connect
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_connect(ably_ws_client_t *transport)
{
    assert(transport != NULL);

    transport->connected      = 0;
    transport->close_sent     = 0;
    transport->close_received = 0;

    ably_error_t err = ABLY_ERR_NETWORK;

    /* TCP connect. */
    int tls_result = mbedtls_net_connect(&transport->net,
                                          transport->host, transport->port_str,
                                          MBEDTLS_NET_PROTO_TCP);
    if (tls_result != 0) {
        ABLY_LOG_E(&transport->log, "TCP connect to %s:%s failed: %d",
                   transport->host, transport->port_str, tls_result);
        return ABLY_ERR_NETWORK;
    }

    /* Switch to non-blocking.  All timeouts are enforced by our poll() wrappers.
     * We never call mbedtls_ssl_conf_read_timeout — the ssl_conf is shared
     * across reconnects and must not be mutated per-connection. */
    mbedtls_net_set_nonblock(&transport->net);

    /* TLS setup. */
    tls_result = mbedtls_ssl_setup(&transport->ssl, &transport->ssl_conf);
    if (tls_result != 0) {
        ABLY_LOG_E(&transport->log, "ssl_setup failed: %d", tls_result);
        goto fail_net;
    }

    /* Non-blocking bio: mbedtls_net_recv (not the timeout variant). */
    mbedtls_ssl_set_bio(&transport->ssl, &transport->net,
                         mbedtls_net_send, mbedtls_net_recv, NULL);
    mbedtls_ssl_set_hostname(&transport->ssl, transport->host);

    /* TLS handshake loop — poll for readability/writability on WANT_READ/WANT_WRITE.
     * RECEIVED_NEW_SESSION_TICKET is a TLS 1.3 post-handshake message; mbedTLS
     * processes it internally and returns this code so the caller can retry. */
    while (1) {
        tls_result = mbedtls_ssl_handshake(&transport->ssl);
        if (tls_result == 0) break;

        if (tls_result == MBEDTLS_ERR_SSL_WANT_READ) {
            int poll_result = tls_poll_for_read(transport->net.fd, (int)transport->timeout_ms);
            if (poll_result <= 0) {
                ABLY_LOG_E(&transport->log, "TLS handshake timed out waiting for read");
                goto fail_ssl;
            }
            continue;
        }
        if (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int poll_result = tls_poll_for_write(transport->net.fd, (int)transport->timeout_ms);
            if (poll_result <= 0) {
                ABLY_LOG_E(&transport->log, "TLS handshake timed out waiting for write");
                goto fail_ssl;
            }
            continue;
        }
        if (tls_result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }

        ABLY_LOG_E(&transport->log, "TLS handshake failed: %d", tls_result);
        goto fail_ssl;
    }

    ABLY_LOG_I(&transport->log, "TLS connected: %s / %s",
               mbedtls_ssl_get_version(&transport->ssl),
               mbedtls_ssl_get_ciphersuite(&transport->ssl));

    /* WebSocket upgrade handshake. */
    err = perform_websocket_upgrade_handshake(transport);
    if (err != ABLY_OK) {
        goto fail_ssl;
    }

    /* Initialise wslay client context. */
    if (transport->wslay_ctx) {
        wslay_event_context_free(transport->wslay_ctx);
        transport->wslay_ctx = NULL;
    }

    tls_result = wslay_event_context_client_init(&transport->wslay_ctx,
                                                   &wslay_callbacks, transport);
    if (tls_result != 0) {
        ABLY_LOG_E(&transport->log, "wslay_event_context_client_init failed: %d", tls_result);
        goto fail_ssl;
    }

    transport->connected = 1;
    ABLY_LOG_I(&transport->log, "WebSocket connected to %s%s",
               transport->host, transport->path);
    return ABLY_OK;

fail_ssl:
    mbedtls_ssl_free(&transport->ssl);
    mbedtls_ssl_init(&transport->ssl);
fail_net:
    mbedtls_net_free(&transport->net);
    mbedtls_net_init(&transport->net);
    return err != ABLY_OK ? err : ABLY_ERR_NETWORK;
}

/* ---------------------------------------------------------------------------
 * Send text frame
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_send_text(ably_ws_client_t *transport,
                                 const char *payload, size_t payload_length)
{
    assert(transport != NULL);
    assert(payload != NULL);

    if (!transport->connected || transport->close_sent) return ABLY_ERR_STATE;

    struct wslay_event_msg message;
    message.opcode      = WSLAY_TEXT_FRAME;
    message.msg         = (const uint8_t *)payload;
    message.msg_length  = payload_length;

    int wslay_result = wslay_event_queue_msg(transport->wslay_ctx, &message);
    if (wslay_result != 0) return ABLY_ERR_NETWORK;

    while (wslay_event_want_write(transport->wslay_ctx)) {
        wslay_result = wslay_event_send(transport->wslay_ctx);
        if (wslay_result != 0) {
            ABLY_LOG_E(&transport->log, "wslay_event_send failed: %d", wslay_result);
            transport->connected = 0;
            return ABLY_ERR_NETWORK;
        }
    }
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Receive (called from service thread event loop, once per iteration)
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_recv_once(ably_ws_client_t *transport, int timeout_ms)
{
    assert(transport != NULL);

    if (!transport->connected)      return ABLY_ERR_NETWORK;
    if (transport->close_received)  return ABLY_ERR_NETWORK;

    /* If mbedTLS has no buffered application data, wait for kernel data. */
    if (mbedtls_ssl_get_bytes_avail(&transport->ssl) == 0) {
        int poll_result = tls_poll_for_read(transport->net.fd,
                                             timeout_ms > 0 ? timeout_ms : 1000);
        if (poll_result == 0) return ABLY_OK;  /* timeout — no data, not an error */
        if (poll_result < 0) {
            transport->connected = 0;
            return ABLY_ERR_NETWORK;
        }
    }

    int wslay_result = wslay_event_recv(transport->wslay_ctx);
    if (wslay_result == 0 || wslay_result == WSLAY_ERR_WOULDBLOCK) return ABLY_OK;

    ABLY_LOG_I(&transport->log, "wslay_event_recv returned %d — disconnected", wslay_result);
    transport->connected = 0;
    return ABLY_ERR_NETWORK;
}

/* ---------------------------------------------------------------------------
 * Close
 * --------------------------------------------------------------------------- */

ably_error_t ably_ws_close(ably_ws_client_t *transport, int timeout_ms)
{
    assert(transport != NULL);
    if (!transport->connected) return ABLY_OK;

    /* Queue a WebSocket close frame and flush it. */
    struct wslay_event_msg close_frame;
    close_frame.opcode     = WSLAY_CONNECTION_CLOSE;
    close_frame.msg        = NULL;
    close_frame.msg_length = 0;

    wslay_event_queue_msg(transport->wslay_ctx, &close_frame);
    transport->close_sent = 1;

    while (wslay_event_want_write(transport->wslay_ctx)) {
        wslay_event_send(transport->wslay_ctx);
    }

    /* Wait for peer's close frame. */
    long deadline_ms = timeout_ms > 0 ? timeout_ms : 5000;
    long waited_ms   = 0;
    while (!transport->close_received && waited_ms < deadline_ms) {
        ably_ws_recv_once(transport, 200);
        waited_ms += 200;
    }

    mbedtls_ssl_close_notify(&transport->ssl);
    mbedtls_ssl_free(&transport->ssl);
    mbedtls_ssl_init(&transport->ssl);
    mbedtls_net_free(&transport->net);
    mbedtls_net_init(&transport->net);
    transport->connected = 0;

    return transport->close_received ? ABLY_OK : ABLY_ERR_TIMEOUT;
}

/* ---------------------------------------------------------------------------
 * Path update (for resume reconnects)
 * --------------------------------------------------------------------------- */

void ably_ws_client_set_path(ably_ws_client_t *transport, const char *path)
{
    assert(transport != NULL);
    assert(path != NULL);
    snprintf(transport->path, sizeof(transport->path), "%s", path);
}
