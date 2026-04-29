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

#include "log.h"
#include "ably/ably.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static int g_log_level = ABLY_LOG_INFO;

void ably_set_log_level(ably_log_level_t level)
{
    g_log_level = (int)level;
}

ably_log_level_t ably_get_log_level(void)
{
    return (ably_log_level_t)g_log_level;
}

static const char *level_str(int level)
{
    switch (level) {
    case 0: return "ERROR";
    case 1: return "WARN ";
    case 2: return "INFO ";
    case 3: return "DEBUG";
    default: return "?    ";
    }
}

static void default_log_cb(int level, const char *file, int line,
                             const char *message, void *user_data)
{
    (void)user_data;
    if (level > g_log_level) return;

    /* Use only the basename of the file path. */
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

#ifdef _WIN32
    const char *bslash = strrchr(base, '\\');
    base = bslash ? bslash + 1 : base;
#endif

    fprintf(stderr, "[ably %s] %s:%d: %s\n", level_str(level), base, line, message);
}

void ably_log_write(const ably_log_ctx_t *ctx, int level,
                     const char *file, int line,
                     const char *fmt, ...)
{
    if (level > g_log_level) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (ctx && ctx->cb) {
        ctx->cb(level, file, line, buf, ctx->user_data);
    } else {
        default_log_cb(level, file, line, buf, NULL);
    }
}
