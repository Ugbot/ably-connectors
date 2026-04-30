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

#ifndef ABLY_WS_CLIENT_H
#define ABLY_WS_CLIENT_H

#include "ably/ably_types.h"
#include "alloc.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

/*
 * WebSocket client for Ably real-time connections.
 *
 * Wraps mbedTLS (TLS + TCP) and wslay (WebSocket RFC 6455 framing).
 *
 * The WebSocket opening handshake is performed synchronously.
 * After that, ably_ws_send() and ably_ws_recv() are blocking calls
 * used by the service thread's event loop.
 *
 * TigerStyle: all buffers pre-allocated.  No allocation on any hot path.
 */

#ifndef ABLY_WS_RECV_BUF_SIZE
#  define ABLY_WS_RECV_BUF_SIZE  (128 * 1024)
#endif
#ifndef ABLY_WS_SEND_BUF_SIZE
#  define ABLY_WS_SEND_BUF_SIZE  (64 * 1024)
#endif
#ifndef ABLY_WS_HOST_MAX
#  define ABLY_WS_HOST_MAX       256
#endif
#ifndef ABLY_WS_PATH_MAX
#  define ABLY_WS_PATH_MAX       1024
#endif
#ifndef ABLY_WS_HANDSHAKE_BUF
#  define ABLY_WS_HANDSHAKE_BUF  (8 * 1024)
#endif

typedef struct ably_ws_client_s ably_ws_client_t;

typedef struct {
    const char *host;
    uint16_t    port;
    const char *path;            /* URL path+query, e.g. "/?v=2&key=..." */
    long        timeout_ms;
    int         tls_verify_peer;
} ably_ws_options_t;

/*
 * Callback invoked by ably_ws_recv_once() when a complete text frame arrives.
 * buf[0..len-1] is the payload (NUL-terminated; buf[len]=='\0').
 * user_data is whatever was passed to ably_ws_client_create().
 */
typedef void (*ably_ws_frame_cb)(const char *buf, size_t len, void *user_data);

/*
 * Create a WebSocket client (does not connect yet).
 * Returns NULL on allocation failure.
 */
ably_ws_client_t *ably_ws_client_create(const ably_ws_options_t *opts,
                                          ably_ws_frame_cb         on_frame,
                                          void                    *user_data,
                                          const ably_allocator_t  *alloc,
                                          const ably_log_ctx_t    *log);

/* Destroy and free resources.  Safe with NULL. */
void ably_ws_client_destroy(ably_ws_client_t *client);

/* Update the URL path used for the next ably_ws_connect() call.
 * path must be NUL-terminated and fit within ABLY_WS_PATH_MAX bytes. */
void ably_ws_client_set_path(ably_ws_client_t *client, const char *path);

/*
 * Connect: TCP + TLS + WebSocket handshake.
 * Blocking.  Returns ABLY_OK on success, ABLY_ERR_NETWORK on failure.
 */
ably_error_t ably_ws_connect(ably_ws_client_t *client);

/*
 * Send a text frame.  Blocking.  Thread-safe only if the caller serialises
 * sends (the service thread is the only sender).
 */
ably_error_t ably_ws_send_text(ably_ws_client_t *client,
                                 const char *payload, size_t len);

/*
 * Receive and dispatch one or more frames.
 * Calls on_frame for each complete text frame received.
 * Returns ABLY_OK normally, ABLY_ERR_NETWORK on disconnection.
 * timeout_ms: how long to wait for data before returning (0 = non-blocking poll).
 */
ably_error_t ably_ws_recv_once(ably_ws_client_t *client, int timeout_ms);

/*
 * Send a WebSocket CLOSE frame and wait for the peer's CLOSE response.
 * timeout_ms: max wait.  Returns ABLY_OK if clean close, ABLY_ERR_TIMEOUT otherwise.
 */
ably_error_t ably_ws_close(ably_ws_client_t *client, int timeout_ms);

/* 1 if connected, 0 if not. */
int ably_ws_is_connected(const ably_ws_client_t *client);

#endif /* ABLY_WS_CLIENT_H */
