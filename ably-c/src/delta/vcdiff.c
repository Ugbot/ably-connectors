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
 * RFC 3284 VCDIFF decoder.
 *
 * Design notes:
 *  - No dynamic allocation; all working state is on the stack.
 *  - The default code table is generated once into a static array guarded by
 *    pthread_once.  After init it is read-only.
 *  - The address cache (near[4] + same[768]) is stack-allocated and reset per
 *    window as the RFC requires.
 *  - COPY instructions that reference already-written target bytes (forward
 *    overlap is forbidden by the spec) are validated byte-by-byte.
 */

#include "vcdiff.h"

#include <string.h>
#include <assert.h>
#include <pthread.h>

/* ---------------------------------------------------------------------------
 * Magic / flags
 * --------------------------------------------------------------------------- */

static const uint8_t VCDIFF_MAGIC[4] = {0xD6, 0xC3, 0xC4, 0x00};

/* Hdr_Indicator */
#define HDR_DECOMP   0x01
#define HDR_CODETBL  0x02

/* Win_Indicator */
#define WIN_SOURCE   0x01
#define WIN_TARGET   0x02
#define WIN_ADLER32  0x04   /* extension: 4-byte checksum appended; we skip it */

/* Delta_Indicator */
#define DELTA_DECOMP_MASK 0x07   /* any secondary compression bit set */

/* ---------------------------------------------------------------------------
 * Code table
 *
 * Default parameters (RFC 3284 §8.4):
 *   S_near = 4, S_same = 3, NUM_MODES = 9
 *
 * Layout (256 entries total):
 *   0         : NOOP/NOOP
 *   1..18     : ADD sizes 0..17
 *   19        : RUN size 0
 *   20..163   : COPY mode 0..8, per mode: size 0 then 4..18  (9×16 = 144)
 *   164..235  : ADD+COPY: add_sz 1..4, modes 0..5, copy_sz 4..6  (4×6×3 = 72)
 *   236..255  : COPY+ADD: modes 0..4, copy_sz 4, add_sz 1..4     (5×4 = 20)
 * --------------------------------------------------------------------------- */

#define S_NEAR     4
#define S_SAME     3
#define NUM_MODES  (2 + S_NEAR + S_SAME)   /* 9 */

#define INST_NOOP  0
#define INST_ADD   1
#define INST_RUN   2
#define INST_COPY  3   /* + mode → 3..11 */

typedef struct { uint8_t t1, s1, m1, t2, s2, m2; } code_entry_t;

static code_entry_t s_code_table[256];
static pthread_once_t s_code_table_once = PTHREAD_ONCE_INIT;

static void build_code_table(void)
{
    int i = 0;

    s_code_table[i++] = (code_entry_t){INST_NOOP, 0, 0, INST_NOOP, 0, 0};

    for (int sz = 0; sz <= 17; sz++)
        s_code_table[i++] = (code_entry_t){INST_ADD, (uint8_t)sz, 0, INST_NOOP, 0, 0};

    s_code_table[i++] = (code_entry_t){INST_RUN, 0, 0, INST_NOOP, 0, 0};

    for (int m = 0; m < NUM_MODES; m++) {
        s_code_table[i++] = (code_entry_t){INST_COPY, 0, (uint8_t)m, INST_NOOP, 0, 0};
        for (int sz = 4; sz <= 18; sz++)
            s_code_table[i++] = (code_entry_t){INST_COPY, (uint8_t)sz, (uint8_t)m,
                                                INST_NOOP, 0, 0};
    }
    /* i == 164 */

    /* ADD+COPY: add_sz 1..4, modes 0..5, copy_sz 4..6 → 72 entries */
    for (int a = 1; a <= 4; a++)
        for (int m = 0; m < NUM_MODES - 3; m++)
            for (int sz = 4; sz <= 6; sz++)
                s_code_table[i++] = (code_entry_t){INST_ADD,  (uint8_t)a,  0,
                                                    INST_COPY, (uint8_t)sz, (uint8_t)m};

    /* COPY+ADD: modes 0..4, copy_sz 4, add_sz 1..4 → 20 entries */
    for (int m = 0; m < NUM_MODES - 4; m++)
        for (int a = 1; a <= 4; a++)
            s_code_table[i++] = (code_entry_t){INST_COPY, 4, (uint8_t)m,
                                                INST_ADD,  (uint8_t)a, 0};

    assert(i == 256);
}

/* ---------------------------------------------------------------------------
 * Variable-length integer (big-endian 7-bit groups, MSB = continuation bit)
 *
 * Returns bytes consumed, or 0 on truncation / overflow.
 * --------------------------------------------------------------------------- */

static int read_varint(const uint8_t *pos, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    int n = 0;
    while (pos + n < end) {
        uint8_t b = pos[n++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) { *out = v; return n; }
        if (n >= 9) return 0;  /* overflow */
    }
    return 0;  /* truncated */
}

/* ---------------------------------------------------------------------------
 * Execute one instruction against the combined source+target window.
 * --------------------------------------------------------------------------- */

static ably_vcdiff_error_t execute_copy(
    uint8_t  inst_mode,
    size_t   size,
    const uint8_t **addr_pos, const uint8_t *addr_end,
    uint32_t near_cache[S_NEAR], int *near_next,
    uint32_t same_cache[S_SAME * 256],
    const uint8_t *src_data, size_t src_len,
    uint8_t  *output, size_t out_cap,
    size_t   *out_pos, size_t window_tgt_start, size_t *tgt_written)
{
    uint32_t addr;
    int n;
    uint64_t a;

    if (inst_mode == 0) {
        /* VCD_SELF: absolute varint */
        n = read_varint(*addr_pos, addr_end, &a);
        if (!n) return ABLY_VCDIFF_ERR_TRUNCATED;
        *addr_pos += n;
        addr = (uint32_t)a;
    } else if (inst_mode == 1) {
        /* VCD_HERE: here - varint */
        n = read_varint(*addr_pos, addr_end, &a);
        if (!n) return ABLY_VCDIFF_ERR_TRUNCATED;
        *addr_pos += n;
        uint32_t here = (uint32_t)(src_len + *tgt_written);
        addr = here - (uint32_t)a;
    } else if (inst_mode < 2 + S_NEAR) {
        /* NEAR: near_cache[mode-2] + varint */
        n = read_varint(*addr_pos, addr_end, &a);
        if (!n) return ABLY_VCDIFF_ERR_TRUNCATED;
        *addr_pos += n;
        addr = near_cache[inst_mode - 2] + (uint32_t)a;
    } else {
        /* SAME: single byte lookup */
        int same_idx = inst_mode - (2 + S_NEAR);
        if (*addr_pos >= addr_end) return ABLY_VCDIFF_ERR_TRUNCATED;
        uint8_t byte = *(*addr_pos)++;
        addr = same_cache[same_idx * 256 + byte];
    }

    /* Update address cache */
    near_cache[*near_next] = addr;
    *near_next = (*near_next + 1) % S_NEAR;
    same_cache[addr % (S_SAME * 256)] = addr;

    if (*out_pos + size > out_cap) return ABLY_VCDIFF_ERR_OUTPUT_FULL;

    for (size_t j = 0; j < size; j++) {
        size_t combined = (size_t)addr + j;
        uint8_t b;
        if (combined < src_len) {
            b = src_data[combined];
        } else {
            size_t tgt_off = combined - src_len;
            if (tgt_off >= *tgt_written) return ABLY_VCDIFF_ERR_INVALID;
            b = output[window_tgt_start + tgt_off];
        }
        output[(*out_pos)++] = b;
        (*tgt_written)++;
    }
    return ABLY_VCDIFF_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

ably_vcdiff_error_t ably_vcdiff_decode(
    const uint8_t *source, size_t  source_len,
    const uint8_t *delta,  size_t  delta_len,
    uint8_t       *output, size_t *output_len)
{
    pthread_once(&s_code_table_once, build_code_table);

    if (!delta || !output || !output_len) return ABLY_VCDIFF_ERR_INVALID;

    const uint8_t *p   = delta;
    const uint8_t *end = delta + delta_len;

    /* ---- File header ---- */
    if ((size_t)(end - p) < 5) return ABLY_VCDIFF_ERR_TRUNCATED;
    if (memcmp(p, VCDIFF_MAGIC, 4) != 0) return ABLY_VCDIFF_ERR_INVALID_MAGIC;
    p += 4;

    uint8_t hdr_flags = *p++;
    if (hdr_flags & HDR_DECOMP)  return ABLY_VCDIFF_ERR_SECONDARY_COMP;
    if (hdr_flags & HDR_CODETBL) return ABLY_VCDIFF_ERR_CUSTOM_TABLE;

    size_t out_pos = 0;
    size_t out_cap = *output_len;

    /* ---- Windows ---- */
    while (p < end) {
        uint8_t win_ind = *p++;

        /* Source segment (for VCD_SOURCE or VCD_TARGET) */
        const uint8_t *seg_data = NULL;
        size_t         seg_len  = 0;

        if (win_ind & (WIN_SOURCE | WIN_TARGET)) {
            uint64_t slen, spos;
            int n;
            n = read_varint(p, end, &slen); if (!n) return ABLY_VCDIFF_ERR_TRUNCATED; p += n;
            n = read_varint(p, end, &spos); if (!n) return ABLY_VCDIFF_ERR_TRUNCATED; p += n;

            if (win_ind & WIN_SOURCE) {
                if (!source || spos + slen > source_len) return ABLY_VCDIFF_ERR_INVALID;
                seg_data = source + (size_t)spos;
                seg_len  = (size_t)slen;
            } else {
                /* VCD_TARGET: reference already-written output */
                if (spos + slen > out_pos) return ABLY_VCDIFF_ERR_INVALID;
                seg_data = output + (size_t)spos;
                seg_len  = (size_t)slen;
            }
        }

        /* delta_length covers the rest of this window */
        uint64_t wlen;
        int n = read_varint(p, end, &wlen);
        if (!n) return ABLY_VCDIFF_ERR_TRUNCATED;
        p += n;
        if ((size_t)(end - p) < (size_t)wlen) return ABLY_VCDIFF_ERR_TRUNCATED;
        const uint8_t *w     = p;
        const uint8_t *w_end = p + (size_t)wlen;
        p = w_end;  /* advance past this window unconditionally */

        /* Parse delta encoding header */
        uint64_t target_len;
        n = read_varint(w, w_end, &target_len); if (!n) return ABLY_VCDIFF_ERR_TRUNCATED; w += n;

        if (w >= w_end) return ABLY_VCDIFF_ERR_TRUNCATED;
        uint8_t delta_ind = *w++;
        if (delta_ind & DELTA_DECOMP_MASK) return ABLY_VCDIFF_ERR_SECONDARY_COMP;

        uint64_t data_len, inst_len, addr_len;
        n = read_varint(w, w_end, &data_len); if (!n) return ABLY_VCDIFF_ERR_TRUNCATED; w += n;
        n = read_varint(w, w_end, &inst_len); if (!n) return ABLY_VCDIFF_ERR_TRUNCATED; w += n;
        n = read_varint(w, w_end, &addr_len); if (!n) return ABLY_VCDIFF_ERR_TRUNCATED; w += n;

        if ((size_t)(w_end - w) < data_len + inst_len + addr_len)
            return ABLY_VCDIFF_ERR_TRUNCATED;

        const uint8_t *data_cur = w;
        const uint8_t *data_end = w + (size_t)data_len;
        const uint8_t *inst_cur = data_end;
        const uint8_t *inst_end = inst_cur + (size_t)inst_len;
        const uint8_t *addr_cur = inst_end;
        const uint8_t *addr_end_p = addr_cur + (size_t)addr_len;

        if (out_pos + (size_t)target_len > out_cap) return ABLY_VCDIFF_ERR_OUTPUT_FULL;

        /* Address cache — reset per window */
        uint32_t near_cache[S_NEAR] = {0};
        int      near_next = 0;
        uint32_t same_cache[S_SAME * 256];
        memset(same_cache, 0, sizeof(same_cache));

        size_t window_tgt_start = out_pos;  /* where this window's target begins */
        size_t tgt_written = 0;

        /* Execute instructions */
        while (inst_cur < inst_end) {
            uint8_t opcode = *inst_cur++;
            const code_entry_t *ce = &s_code_table[opcode];

            for (int pass = 0; pass < 2; pass++) {
                uint8_t itype = (pass == 0) ? ce->t1 : ce->t2;
                uint8_t isize = (pass == 0) ? ce->s1 : ce->s2;
                uint8_t imode = (pass == 0) ? ce->m1 : ce->m2;

                if (itype == INST_NOOP) continue;

                size_t size = isize;
                if (isize == 0) {
                    uint64_t sz;
                    n = read_varint(inst_cur, inst_end, &sz);
                    if (!n) return ABLY_VCDIFF_ERR_TRUNCATED;
                    inst_cur += n;
                    size = (size_t)sz;
                }

                if (itype == INST_ADD) {
                    if ((size_t)(data_end - data_cur) < size) return ABLY_VCDIFF_ERR_INVALID;
                    if (out_pos + size > out_cap) return ABLY_VCDIFF_ERR_OUTPUT_FULL;
                    memcpy(output + out_pos, data_cur, size);
                    data_cur  += size;
                    out_pos   += size;
                    tgt_written += size;

                } else if (itype == INST_RUN) {
                    if (data_cur >= data_end) return ABLY_VCDIFF_ERR_INVALID;
                    uint8_t run_byte = *data_cur++;
                    if (out_pos + size > out_cap) return ABLY_VCDIFF_ERR_OUTPUT_FULL;
                    memset(output + out_pos, run_byte, size);
                    out_pos     += size;
                    tgt_written += size;

                } else {
                    /* COPY */
                    ably_vcdiff_error_t err = execute_copy(
                        imode, size,
                        &addr_cur, addr_end_p,
                        near_cache, &near_next, same_cache,
                        seg_data, seg_len,
                        output, out_cap,
                        &out_pos, window_tgt_start, &tgt_written);
                    if (err != ABLY_VCDIFF_OK) return err;
                }
            }
        }

        if (tgt_written != (size_t)target_len) return ABLY_VCDIFF_ERR_INVALID;
    }

    *output_len = out_pos;
    return ABLY_VCDIFF_OK;
}
