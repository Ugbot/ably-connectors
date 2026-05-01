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

#ifndef ABLY_REST_CLIENT_INTERNAL_H
#define ABLY_REST_CLIENT_INTERNAL_H

#include "ably/ably_rest.h"
#include "http/http_client.h"
#include "alloc.h"
#include "log.h"

struct ably_rest_client_s {
    ably_http_client_t *http;
    ably_allocator_t    alloc;
    ably_log_ctx_t      log;
    long                last_http_status;
    ably_encoding_t     encoding;

    /* Pre-allocated body buffer for encoding publish payloads. */
    char  *body_buf;             /* ABLY_HTTP_REQUEST_BUF_SIZE bytes */

    /* Raw api_key stored for token-request signing (HMAC-SHA256). */
    char   api_key[ABLY_MAX_KEY_LEN];
};

#endif /* ABLY_REST_CLIENT_INTERNAL_H */
