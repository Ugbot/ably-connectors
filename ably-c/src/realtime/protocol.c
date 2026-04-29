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
 * Shared error string implementation; codec implementations live in
 * protocol_json.c and protocol_msgpack.c.
 */

#include "protocol.h"
#include "ably/ably_types.h"

const char *ably_error_str(ably_error_t err)
{
    switch (err) {
    case ABLY_OK:              return "OK";
    case ABLY_ERR_NOMEM:       return "out of memory";
    case ABLY_ERR_INVALID_ARG: return "invalid argument";
    case ABLY_ERR_AUTH:        return "authentication failure";
    case ABLY_ERR_NETWORK:     return "network error";
    case ABLY_ERR_HTTP:        return "unexpected HTTP status";
    case ABLY_ERR_PROTOCOL:    return "protocol error";
    case ABLY_ERR_TIMEOUT:     return "operation timed out";
    case ABLY_ERR_STATE:       return "invalid state for operation";
    case ABLY_ERR_THREAD:      return "thread error";
    case ABLY_ERR_CAPACITY:    return "capacity limit reached";
    case ABLY_ERR_INTERNAL:    return "internal error";
    default:                   return "unknown error";
    }
}
