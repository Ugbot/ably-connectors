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

size_t ably_base64_decode_max_len(size_t encoded_len)
{
    return (encoded_len / 4) * 3 + 3;
}

/* Decode table: -1 = invalid, -2 = padding '=', 0..63 = value */
static const int8_t b64_dec[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0x00-0x0F */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0x10-0x1F */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,  /* 0x20-0x2F  '+','/' */
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,  /* 0x30-0x3F  '0'-'9','=' */
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* 0x40-0x4F  'A'-'N' */
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,  /* 0x50-0x5F  'O'-'Z' */
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* 0x60-0x6F  'a'-'n' */
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,  /* 0x70-0x7F  'o'-'z' */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

int ably_base64_decode(uint8_t *dst, size_t dst_len, size_t *olen,
                        const char *src, size_t src_len)
{
    size_t o = 0;
    size_t i = 0;

    /* Skip trailing whitespace / nulls */
    while (src_len > 0 && (src[src_len - 1] == '\0' || src[src_len - 1] == '\n'
                            || src[src_len - 1] == '\r' || src[src_len - 1] == ' '))
        src_len--;

    while (i + 3 < src_len) {
        int8_t a = b64_dec[(uint8_t)src[i]];
        int8_t b = b64_dec[(uint8_t)src[i+1]];
        int8_t c = b64_dec[(uint8_t)src[i+2]];
        int8_t d = b64_dec[(uint8_t)src[i+3]];
        i += 4;

        if (a < 0 || b < 0) return -1;

        if (o + 1 > dst_len) return -1;
        dst[o++] = (uint8_t)((a << 2) | (b >> 4));

        if (c == -2) break;  /* padding */
        if (c < 0)  return -1;
        if (o + 1 > dst_len) return -1;
        dst[o++] = (uint8_t)((b << 4) | (c >> 2));

        if (d == -2) break;  /* padding */
        if (d < 0)  return -1;
        if (o + 1 > dst_len) return -1;
        dst[o++] = (uint8_t)((c << 6) | d);
    }

    *olen = o;
    return 0;
}
