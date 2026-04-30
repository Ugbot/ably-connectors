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

#ifndef ABLY_HTTP_CLIENT_H
#define ABLY_HTTP_CLIENT_H

#include "ably/ably_types.h"
#include "alloc.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal HTTP/1.1 client for HTTPS POST/GET, built on top of mbedTLS.
 *
 * Not a general-purpose HTTP library — supports only what Ably's REST API
 * needs:
 *   - HTTPS (TLS 1.2/1.3 via mbedTLS)
 *   - POST with a Content-Type: application/json or application/x-msgpack body
 *   - GET (for future use: channel history, channel status)
 *   - Basic authentication ("Authorization: Basic <base64>")
 *   - A single synchronous request/response cycle per call
 *
 * One ably_http_client_t handles one request at a time.  It is not thread-safe;
 * callers must ensure serialised access.
 *
 * TigerStyle: all buffers are fixed-size and pre-allocated at create time.
 * No allocation occurs on any hot path (i.e., during a request).
 */

/* Maximum sizes for pre-allocated fixed buffers. */
#ifndef ABLY_HTTP_REQUEST_BUF_SIZE
#  define ABLY_HTTP_REQUEST_BUF_SIZE   (64 * 1024)   /* outbound request buffer  */
#endif
#ifndef ABLY_HTTP_RESPONSE_BUF_SIZE
#  define ABLY_HTTP_RESPONSE_BUF_SIZE  (64 * 1024)   /* inbound response buffer  */
#endif
#ifndef ABLY_HTTP_HOST_MAX
#  define ABLY_HTTP_HOST_MAX           256
#endif
#ifndef ABLY_HTTP_AUTH_HEADER_MAX
#  define ABLY_HTTP_AUTH_HEADER_MAX    512            /* "Authorization: Basic <b64>" */
#endif

typedef struct ably_http_client_s ably_http_client_t;

/* Options for creating an HTTP client. */
typedef struct {
    const char *host;            /* e.g. "rest.ably.io"   */
    uint16_t    port;            /* e.g. 443              */
    long        timeout_ms;      /* per-request timeout   */
    int         tls_verify_peer; /* 1 = verify (default)  */
} ably_http_options_t;

/*
 * Create an HTTP client.  Allocates internal buffers via the provided
 * allocator (or system malloc if NULL).
 * Returns NULL on allocation failure.
 */
ably_http_client_t *ably_http_client_create(const ably_http_options_t  *opts,
                                              const char                 *auth_header,
                                              const ably_allocator_t     *alloc,
                                              const ably_log_ctx_t       *log);

/* Destroy and free all resources.  Safe to call with NULL. */
void ably_http_client_destroy(ably_http_client_t *client);

/*
 * Perform a synchronous HTTPS POST.
 *
 *   path         — URL path including leading '/', e.g. "/channels/foo/messages"
 *   content_type — e.g. "application/json"
 *   body         — request body bytes
 *   body_len     — length of body
 *
 * On return:
 *   *http_status  — HTTP status code (e.g. 201), or 0 on transport error
 *
 * Returns ABLY_OK if a response was received (even an error response).
 * Returns ABLY_ERR_NETWORK / ABLY_ERR_TIMEOUT on transport failure.
 */
ably_error_t ably_http_post(ably_http_client_t *client,
                              const char         *path,
                              const char         *content_type,
                              const uint8_t      *body,
                              size_t              body_len,
                              long               *http_status);

/*
 * Perform a synchronous HTTPS GET.
 *
 *   path          — URL path + query, e.g. "/channels/my-ch/messages?limit=100"
 *
 * On return:
 *   *http_status     — HTTP status code, or 0 on transport error
 *   *resp_body_out   — pointer into the client's internal response buffer;
 *                      NUL-terminated, valid until the next HTTP call.
 *   *resp_body_len   — byte length of the body (excluding NUL)
 *
 * resp_body_out and resp_body_len may be NULL if not needed.
 *
 * Returns ABLY_OK if a response was received.
 * Returns ABLY_ERR_NETWORK / ABLY_ERR_TIMEOUT on transport failure.
 */
ably_error_t ably_http_get(ably_http_client_t  *client,
                             const char          *path,
                             long                *http_status,
                             const char         **resp_body_out,
                             size_t              *resp_body_len);

#endif /* ABLY_HTTP_CLIENT_H */
