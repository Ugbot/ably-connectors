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
 * ably.h — single umbrella include for the ably-c library.
 *
 * Usage:
 *   #include <ably/ably.h>
 */

#ifndef ABLY_H
#define ABLY_H

#include "ably_allocator.h"
#include "ably_types.h"
#include "ably_rest.h"
#include "ably_realtime.h"

/* Library version (also generated into ably_version.h by CMake). */
#define ABLY_C_VERSION_MAJOR  0
#define ABLY_C_VERSION_MINOR  1
#define ABLY_C_VERSION_PATCH  0
#define ABLY_C_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global log level applied to all clients that have not registered a
 * per-client log callback.
 *   0 = ERROR  1 = WARN  2 = INFO  3 = DEBUG
 * Default: 2 (INFO).
 */
typedef enum {
    ABLY_LOG_ERROR = 0,
    ABLY_LOG_WARN  = 1,
    ABLY_LOG_INFO  = 2,
    ABLY_LOG_DEBUG = 3,
} ably_log_level_t;

void ably_set_log_level(ably_log_level_t level);
ably_log_level_t ably_get_log_level(void);

#ifdef __cplusplus
}
#endif
#endif /* ABLY_H */
