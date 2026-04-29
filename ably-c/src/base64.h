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

#ifndef ABLY_BASE64_H
#define ABLY_BASE64_H

#include <stddef.h>
#include <stdint.h>

/*
 * RFC 4648 §4 base64 encoding.
 *
 * ably_base64_encode_len(src_len)  — returns the number of bytes required in
 *   the output buffer (including the NUL terminator).
 *
 * ably_base64_encode(dst, dst_len, src, src_len) — encodes src into dst.
 *   dst_len must be >= ably_base64_encode_len(src_len).
 *   Returns the number of characters written (excluding NUL).
 *
 * Used to build the "Authorization: Basic <b64>" header.
 * Ably Basic auth encodes the full API key string ("keyId:keySecret").
 */

size_t ably_base64_encode_len(size_t src_len);

size_t ably_base64_encode(char *dst, size_t dst_len,
                           const uint8_t *src, size_t src_len);

#endif /* ABLY_BASE64_H */
