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
 * Unit tests for:
 *  - src/delta/vcdiff.c  (RFC 3284 decoder)
 *  - src/base64.c        (decode path)
 *
 * Test vectors are hand-constructed from the RFC 3284 spec and verified
 * against the Python VCDIFF reference in scripts/gen_test_vectors.py.
 *
 * VCDIFF format used in all tests:
 *   magic(4) + hdr_indicator(1) + windows...
 * Each window:
 *   win_indicator(1) [+ src_len + src_pos if VCD_SOURCE]
 *   + delta_length(varint) + target_len(varint) + delta_indicator(1)
 *   + data_len(varint) + inst_len(varint) + addr_len(varint)
 *   + data_section + inst_section + addr_section
 *
 * Code table indices used (open-vcdiff layout: ADD sizes 0..16, 17 entries):
 *   Entry 0          = NOOP
 *   Entries 1..17    = ADD size 0..16
 *   Entry 18         = RUN size 0
 *   Entry 19         = COPY size 0 mode 0  (varint size follows)
 *   Entry 20         = COPY size 4 mode 0
 *   Entry 21         = COPY size 5 mode 0
 *   ...
 */

#include "../src/delta/vcdiff.h"
#include "../src/base64.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
        failures++; \
    } else { \
        printf("PASS: %s\n", (msg)); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * VCDIFF test vectors (hand-computed, verified with Python reference)
 * --------------------------------------------------------------------------- */

/*
 * Test 1: no-source window, ADD 5 bytes → "hello"
 *
 * d6c3c400  magic
 * 00        Hdr_Indicator
 * 00        Win_Indicator (no source)
 * 0b        delta_length = 11
 *   05      target_window_length = 5
 *   00      Delta_Indicator
 *   05      data_section_length = 5
 *   01      inst_section_length = 1
 *   00      addr_section_length = 0
 *   68656c6c6f  data: "hello"
 *   06      inst: ADD(5) = entry index 6
 */
static const uint8_t TV1_DELTA[] = {
    0xD6, 0xC3, 0xC4, 0x00,  /* magic */
    0x00,                     /* Hdr_Indicator */
    0x00,                     /* Win_Indicator */
    0x0B,                     /* delta_length = 11 */
    0x05,                     /* target_window_length = 5 */
    0x00,                     /* Delta_Indicator */
    0x05,                     /* data_section_length = 5 */
    0x01,                     /* inst_section_length = 1 */
    0x00,                     /* addr_section_length = 0 */
    0x68, 0x65, 0x6C, 0x6C, 0x6F,  /* data: "hello" */
    0x06,                     /* ADD(5): entry 1+5 = 6 */
};

/*
 * Test 2: VCD_SOURCE window, source="hello", target="hello world"
 *
 * d6c3c400  magic
 * 00        Hdr_Indicator
 * 01        Win_Indicator (VCD_SOURCE)
 * 05        source_segment_length = 5
 * 00        source_segment_position = 0
 * 0e        delta_length = 14
 *   0b      target_window_length = 11
 *   00      Delta_Indicator
 *   06      data_section_length = 6
 *   02      inst_section_length = 2
 *   01      addr_section_length = 1
 *   20776f726c64  data: " world"
 *   15      inst: COPY(5, mode=0) = entry 21 = 0x15
 *   07      inst: ADD(6) = entry 7 = 0x07
 *   00      addr: 0 (source offset for COPY)
 */
static const uint8_t TV2_SOURCE[] = "hello";
static const uint8_t TV2_DELTA[]  = {
    0xD6, 0xC3, 0xC4, 0x00,
    0x00,
    0x01,                    /* VCD_SOURCE */
    0x05,                    /* source_segment_length = 5 */
    0x00,                    /* source_segment_position = 0 */
    0x0E,                    /* delta_length = 14 */
    0x0B,                    /* target_window_length = 11 */
    0x00,
    0x06,                    /* data_section_length = 6 */
    0x02,                    /* inst_section_length = 2 */
    0x01,                    /* addr_section_length = 1 */
    0x20, 0x77, 0x6F, 0x72, 0x6C, 0x64,  /* " world" */
    0x15,                    /* COPY(5, mode=0) = entry 21 */
    0x07,                    /* ADD(6) */
    0x00,                    /* addr = 0 */
};

/*
 * Test 3: VCD_SOURCE window, source="hello", target="world" (ADD only, no COPY)
 *
 * d6c3c400 00 01 05 00 0b 05 00 05 01 00 776f726c64 06
 */
static const uint8_t TV3_SOURCE[] = "hello";
static const uint8_t TV3_DELTA[]  = {
    0xD6, 0xC3, 0xC4, 0x00,
    0x00,
    0x01,                    /* VCD_SOURCE */
    0x05,
    0x00,
    0x0B,                    /* delta_length = 11 */
    0x05,                    /* target_window_length = 5 */
    0x00,
    0x05,                    /* data = 5 */
    0x01,                    /* inst = 1 */
    0x00,                    /* addr = 0 */
    0x77, 0x6F, 0x72, 0x6C, 0x64,  /* "world" */
    0x06,                    /* ADD(5) */
};

/*
 * Test 4: invalid magic → ABLY_VCDIFF_ERR_INVALID_MAGIC
 */
static const uint8_t TV4_BAD[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};

/*
 * Test 5: secondary compression requested → ABLY_VCDIFF_ERR_SECONDARY_COMP
 *
 * Hdr_Indicator bit 0 set.
 */
static const uint8_t TV5_DECOMP[] = {
    0xD6, 0xC3, 0xC4, 0x00,
    0x01,   /* HDR_DECOMP */
};

/*
 * Test 6: truncated delta → ABLY_VCDIFF_ERR_TRUNCATED
 */
static const uint8_t TV6_TRUNC[] = {0xD6, 0xC3, 0xC4, 0x00};

/*
 * Test 7: RUN instruction.
 *
 * No source, target = "aaaaa" (RUN(5, 'a'))
 * RUN opcode = 19 (entry 19), size 0 = variable, so:
 *   inst section = [0x12 (18), varint(5)] = [0x12, 0x05]
 *   data section = ['a'] = [0x61]  (RUN byte comes from data section)
 */
static const uint8_t TV7_DELTA[] = {
    0xD6, 0xC3, 0xC4, 0x00,
    0x00,
    0x00,                    /* no source */
    0x08,                    /* delta_length = 8 */
    0x05,                    /* target_window_length = 5 */
    0x00,
    0x01,                    /* data_section_length = 1 */
    0x02,                    /* inst_section_length = 2 */
    0x00,
    0x61,                    /* data: 'a' */
    0x12, 0x05,              /* inst: RUN(0=var) size=5, entry 18 = 0x12 */
};

/*
 * Test 8: Ably wire format — delta from "ably-c-delta-test-payload-common-prefix-v1 #1"
 *                            to   "ably-c-delta-test-payload-common-prefix-v1 #2"
 *
 * This is an exact reconstruction of the VCDIFF bytes captured from the Ably
 * realtime server (open-vcdiff encoder, no checksum on this window).
 *
 *   win_indicator = 0x01 (VCD_SOURCE)
 *   slen = 44, spos = 0  → source segment = common prefix "…common-prefix-v1 #"
 *   delta_length = 0x0A (10)
 *     target_len = 0x2D (45)
 *     delta_ind  = 0x00
 *     data_len   = 0x01, inst_len = 0x03, addr_len = 0x01
 *     data:  32          ('2')
 *     inst:  13 2C 02   (COPY mode0 var=44, ADD(1))  ← entry 19 = COPY mode 0
 *     addr:  00          (VCD_SELF addr=0)
 */
static const uint8_t TV8_SOURCE[] =
    "ably-c-delta-test-payload-common-prefix-v1 #1";   /* 45 bytes */
static const uint8_t TV8_DELTA[] = {
    0xD6, 0xC3, 0xC4, 0x00,  /* magic */
    0x00,                    /* Hdr_Indicator */
    0x01,                    /* Win_Indicator: VCD_SOURCE */
    0x2C,                    /* source_segment_length = 44 */
    0x00,                    /* source_segment_position = 0 */
    0x0A,                    /* delta_length = 10 */
    0x2D,                    /* target_window_length = 45 */
    0x00,                    /* Delta_Indicator */
    0x01,                    /* data_section_length = 1 */
    0x03,                    /* inst_section_length = 3 */
    0x01,                    /* addr_section_length = 1 */
    0x32,                    /* data: '2' */
    0x13, 0x2C,              /* inst: COPY(mode=0, var-size=44) */
    0x02,                    /* inst: ADD(1) */
    0x00,                    /* addr: 0 (VCD_SELF, start of source segment) */
};

/* ---------------------------------------------------------------------------
 * VCDIFF tests
 * --------------------------------------------------------------------------- */

static void test_vcdiff_add_only(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        NULL, 0,
        TV1_DELTA, sizeof(TV1_DELTA),
        out, &olen);
    CHECK(err  == ABLY_VCDIFF_OK,  "TV1 decode OK");
    CHECK(olen == 5,               "TV1 output length = 5");
    CHECK(memcmp(out, "hello", 5) == 0, "TV1 output = 'hello'");
}

static void test_vcdiff_copy_add(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        TV2_SOURCE, 5,
        TV2_DELTA, sizeof(TV2_DELTA),
        out, &olen);
    CHECK(err  == ABLY_VCDIFF_OK,    "TV2 decode OK");
    CHECK(olen == 11,                "TV2 output length = 11");
    CHECK(memcmp(out, "hello world", 11) == 0, "TV2 output = 'hello world'");
}

static void test_vcdiff_add_with_source(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        TV3_SOURCE, 5,
        TV3_DELTA, sizeof(TV3_DELTA),
        out, &olen);
    CHECK(err  == ABLY_VCDIFF_OK,   "TV3 decode OK");
    CHECK(olen == 5,                "TV3 output length = 5");
    CHECK(memcmp(out, "world", 5) == 0, "TV3 output = 'world'");
}

static void test_vcdiff_run_instruction(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        NULL, 0,
        TV7_DELTA, sizeof(TV7_DELTA),
        out, &olen);
    CHECK(err  == ABLY_VCDIFF_OK,    "TV7 RUN decode OK");
    CHECK(olen == 5,                 "TV7 output length = 5");
    CHECK(memcmp(out, "aaaaa", 5) == 0, "TV7 output = 'aaaaa'");
}

static void test_vcdiff_invalid_magic(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        NULL, 0,
        TV4_BAD, sizeof(TV4_BAD),
        out, &olen);
    CHECK(err == ABLY_VCDIFF_ERR_INVALID_MAGIC, "bad magic → INVALID_MAGIC");
}

static void test_vcdiff_secondary_comp_rejected(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        NULL, 0,
        TV5_DECOMP, sizeof(TV5_DECOMP),
        out, &olen);
    CHECK(err == ABLY_VCDIFF_ERR_SECONDARY_COMP,
          "secondary compression → SECONDARY_COMP");
}

static void test_vcdiff_truncated(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        NULL, 0,
        TV6_TRUNC, sizeof(TV6_TRUNC),
        out, &olen);
    CHECK(err == ABLY_VCDIFF_ERR_TRUNCATED, "truncated stream → TRUNCATED");
}

static void test_vcdiff_output_too_small(void)
{
    uint8_t out[2];
    size_t  olen = sizeof(out);  /* smaller than 5-byte output */
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        NULL, 0,
        TV1_DELTA, sizeof(TV1_DELTA),
        out, &olen);
    CHECK(err == ABLY_VCDIFF_ERR_OUTPUT_FULL, "small buffer → OUTPUT_FULL");
}

static void test_vcdiff_ably_wire_format(void)
{
    uint8_t out[64];
    size_t  olen = sizeof(out);
    ably_vcdiff_error_t err = ably_vcdiff_decode(
        TV8_SOURCE, sizeof(TV8_SOURCE) - 1,   /* -1: exclude NUL */
        TV8_DELTA,  sizeof(TV8_DELTA),
        out, &olen);
    CHECK(err  == ABLY_VCDIFF_OK, "TV8 Ably wire format decode OK");
    CHECK(olen == 45,             "TV8 output length = 45");
    CHECK(memcmp(out, "ably-c-delta-test-payload-common-prefix-v1 #2", 45) == 0,
          "TV8 output = '...#2'");
}

static void test_vcdiff_sequential_decode(void)
{
    /* Simulate an Ably delta sequence:
     *   message 0: full "hello"   (TV1)
     *   message 1: delta → "hello world"  (TV2, source = "hello")
     *   message 2: delta → "world"         (TV3, source = "hello")  */

    uint8_t buf0[64], buf1[64], buf2[64];
    size_t  len0 = sizeof(buf0), len1 = sizeof(buf1), len2 = sizeof(buf2);

    CHECK(ably_vcdiff_decode(NULL, 0, TV1_DELTA, sizeof(TV1_DELTA), buf0, &len0) == ABLY_VCDIFF_OK,
          "seq: msg0 decode OK");
    CHECK(len0 == 5 && memcmp(buf0, "hello", 5) == 0, "seq: msg0 = 'hello'");

    CHECK(ably_vcdiff_decode(buf0, len0, TV2_DELTA, sizeof(TV2_DELTA), buf1, &len1) == ABLY_VCDIFF_OK,
          "seq: msg1 decode OK");
    CHECK(len1 == 11 && memcmp(buf1, "hello world", 11) == 0, "seq: msg1 = 'hello world'");

    CHECK(ably_vcdiff_decode(buf0, len0, TV3_DELTA, sizeof(TV3_DELTA), buf2, &len2) == ABLY_VCDIFF_OK,
          "seq: msg2 decode OK");
    CHECK(len2 == 5 && memcmp(buf2, "world", 5) == 0, "seq: msg2 = 'world'");
}

/* ---------------------------------------------------------------------------
 * Base64 decode tests
 * --------------------------------------------------------------------------- */

static void test_base64_roundtrip(void)
{
    /* Encode "hello", then decode back */
    const uint8_t src[] = "hello";
    char encoded[16];
    ably_base64_encode(encoded, sizeof(encoded), src, 5);

    uint8_t decoded[16];
    size_t  dlen = 0;
    int rc = ably_base64_decode(decoded, sizeof(decoded), &dlen,
                                 encoded, strlen(encoded));
    CHECK(rc == 0,   "base64 decode returns 0");
    CHECK(dlen == 5, "base64 decode length = 5");
    CHECK(memcmp(decoded, "hello", 5) == 0, "base64 decode matches original");
}

static void test_base64_known_vector(void)
{
    /* RFC 4648 §10 test vectors */
    struct { const char *enc; const char *raw; size_t raw_len; } cases[] = {
        {"",         "",       0},
        {"Zg==",     "f",      1},
        {"Zm8=",     "fo",     2},
        {"Zm9v",     "foo",    3},
        {"Zm9vYg==", "foob",   4},
        {"Zm9vYmE=", "fooba",  5},
        {"Zm9vYmFy", "foobar", 6},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint8_t out[16]; size_t olen = 0;
        int rc = ably_base64_decode(out, sizeof(out), &olen,
                                     cases[i].enc, strlen(cases[i].enc));
        CHECK(rc == 0, "b64 decode returns 0");
        CHECK(olen == cases[i].raw_len, "b64 decode length matches");
        CHECK(memcmp(out, cases[i].raw, cases[i].raw_len) == 0, "b64 decode value matches");
    }
}

static void test_base64_decode_vcdiff_payload(void)
{
    /* Encode TV2_DELTA as base64, then decode and apply VCDIFF */
    char   b64[256];
    size_t b64_len = ably_base64_encode(b64, sizeof(b64),
                                         TV2_DELTA, sizeof(TV2_DELTA));
    CHECK(b64_len > 0, "encode TV2_DELTA to base64");

    uint8_t raw[256]; size_t raw_len = 0;
    int rc = ably_base64_decode(raw, sizeof(raw), &raw_len, b64, b64_len);
    CHECK(rc == 0, "decode b64 back to raw");
    CHECK(raw_len == sizeof(TV2_DELTA), "roundtrip length matches");
    CHECK(memcmp(raw, TV2_DELTA, sizeof(TV2_DELTA)) == 0, "roundtrip bytes match");

    /* Now apply as VCDIFF */
    uint8_t out[64]; size_t olen = sizeof(out);
    CHECK(ably_vcdiff_decode(TV2_SOURCE, 5, raw, raw_len, out, &olen) == ABLY_VCDIFF_OK,
          "VCDIFF decode after base64 roundtrip OK");
    CHECK(olen == 11 && memcmp(out, "hello world", 11) == 0,
          "VCDIFF result correct after base64 roundtrip");
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------- */

int main(void)
{
    test_vcdiff_add_only();
    test_vcdiff_copy_add();
    test_vcdiff_add_with_source();
    test_vcdiff_run_instruction();
    test_vcdiff_invalid_magic();
    test_vcdiff_secondary_comp_rejected();
    test_vcdiff_truncated();
    test_vcdiff_output_too_small();
    test_vcdiff_ably_wire_format();
    test_vcdiff_sequential_decode();

    test_base64_roundtrip();
    test_base64_known_vector();
    test_base64_decode_vcdiff_payload();

    if (failures == 0) {
        printf("All delta/base64 tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
