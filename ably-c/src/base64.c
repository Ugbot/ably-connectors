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

#include "base64.h"
#include <assert.h>

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ably_base64_encode_len(size_t src_len)
{
    /* 4 output chars per 3 input bytes, padded to multiple of 4, plus NUL. */
    return ((src_len + 2) / 3) * 4 + 1;
}

size_t ably_base64_encode(char *dst, size_t dst_len,
                           const uint8_t *src, size_t src_len)
{
    assert(dst != NULL);
    assert(src != NULL || src_len == 0);
    assert(dst_len >= ably_base64_encode_len(src_len));

    size_t i = 0, o = 0;

    while (i + 2 < src_len) {
        uint32_t triple = ((uint32_t)src[i] << 16)
                        | ((uint32_t)src[i + 1] << 8)
                        |  (uint32_t)src[i + 2];
        dst[o++] = b64_table[(triple >> 18) & 0x3F];
        dst[o++] = b64_table[(triple >> 12) & 0x3F];
        dst[o++] = b64_table[(triple >>  6) & 0x3F];
        dst[o++] = b64_table[(triple >>  0) & 0x3F];
        i += 3;
    }

    if (i < src_len) {
        uint32_t triple = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) triple |= (uint32_t)src[i + 1] << 8;

        dst[o++] = b64_table[(triple >> 18) & 0x3F];
        dst[o++] = b64_table[(triple >> 12) & 0x3F];
        dst[o++] = (i + 1 < src_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        dst[o++] = '=';
    }

    dst[o] = '\0';
    return o;
}
