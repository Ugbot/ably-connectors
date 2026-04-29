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
 * Minimal RFC 3284 VCDIFF decoder.
 *
 * Supports: standard default code table (S_near=4, S_same=3), VCD_SOURCE and
 * VCD_TARGET windows, ADD / RUN / COPY instructions, Adler-32 extension byte
 * (skipped, not verified).
 *
 * Does NOT support: secondary compression (Delta_Indicator != 0), custom code
 * tables (Hdr_Indicator VCD_CODETBL bit).
 *
 * No dynamic allocation.  The caller supplies the output buffer.
 */

#ifndef ABLY_VCDIFF_H
#define ABLY_VCDIFF_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ABLY_VCDIFF_OK                = 0,
    ABLY_VCDIFF_ERR_INVALID_MAGIC = 1,  /* not a VCDIFF stream             */
    ABLY_VCDIFF_ERR_SECONDARY_COMP= 2,  /* secondary compression requested */
    ABLY_VCDIFF_ERR_CUSTOM_TABLE  = 3,  /* custom code table requested     */
    ABLY_VCDIFF_ERR_TRUNCATED     = 4,  /* delta bytes cut off unexpectedly */
    ABLY_VCDIFF_ERR_INVALID       = 5,  /* corrupt / invalid delta data    */
    ABLY_VCDIFF_ERR_OUTPUT_FULL   = 6,  /* output buffer too small         */
} ably_vcdiff_error_t;

/*
 * Decode a VCDIFF delta.
 *
 *   source / source_len  — the base (previous message bytes); may be NULL/0
 *                          for windows without a source segment.
 *   delta  / delta_len   — raw VCDIFF bytes (NOT base64-encoded).
 *   output               — caller-provided output buffer.
 *   output_len           — in: buffer capacity; out: bytes written on success.
 *
 * Returns ABLY_VCDIFF_OK on success.
 */
ably_vcdiff_error_t ably_vcdiff_decode(
    const uint8_t *source, size_t  source_len,
    const uint8_t *delta,  size_t  delta_len,
    uint8_t       *output, size_t *output_len);

#endif /* ABLY_VCDIFF_H */
