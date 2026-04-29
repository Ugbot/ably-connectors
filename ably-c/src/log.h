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

#ifndef ABLY_LOG_H
#define ABLY_LOG_H

#include "ably/ably_types.h"

/* Internal log context carried by each client. */
typedef struct {
    ably_log_cb  cb;
    void        *user_data;
} ably_log_ctx_t;

void ably_log_write(const ably_log_ctx_t *ctx, int level,
                     const char *file, int line,
                     const char *fmt, ...);

/* Convenience macros — pass ctx as the first argument. */
#define ABLY_LOG_E(ctx, ...) ably_log_write((ctx), 0, __FILE__, __LINE__, __VA_ARGS__)
#define ABLY_LOG_W(ctx, ...) ably_log_write((ctx), 1, __FILE__, __LINE__, __VA_ARGS__)
#define ABLY_LOG_I(ctx, ...) ably_log_write((ctx), 2, __FILE__, __LINE__, __VA_ARGS__)
#define ABLY_LOG_D(ctx, ...) ably_log_write((ctx), 3, __FILE__, __LINE__, __VA_ARGS__)

#endif /* ABLY_LOG_H */
