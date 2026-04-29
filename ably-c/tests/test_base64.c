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
 * RFC 4648 §10 test vectors plus Ably API key encoding check.
 */

#include "../src/base64.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void check(const char *input, const char *expected)
{
    size_t src_len = strlen(input);
    size_t buf_len = ably_base64_encode_len(src_len);
    char  *buf     = malloc(buf_len);
    if (!buf) { fprintf(stderr, "malloc failed\n"); exit(1); }

    size_t written = ably_base64_encode(buf, buf_len,
                                         (const unsigned char *)input, src_len);

    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "FAIL: encode(\"%s\") = \"%s\", want \"%s\"\n",
                input, buf, expected);
        failures++;
    } else {
        printf("PASS: encode(\"%s\") = \"%s\"\n", input, buf);
    }

    /* Verify returned length matches strlen of result. */
    if (written != strlen(expected)) {
        fprintf(stderr, "FAIL: encode(\"%s\") returned length %zu, want %zu\n",
                input, written, strlen(expected));
        failures++;
    }

    free(buf);
}

int main(void)
{
    /* RFC 4648 §10 standard test vectors */
    check("",       "");
    check("f",      "Zg==");
    check("fo",     "Zm8=");
    check("foo",    "Zm9v");
    check("foob",   "Zm9vYg==");
    check("fooba",  "Zm9vYmE=");
    check("foobar", "Zm9vYmFy");

    /* Ably API key encoding: the library sends "Authorization: Basic <b64(key)>"
     * where the full key string is base64-encoded. */
    check("appId.keyId:keySecret", "YXBwSWQua2V5SWQ6a2V5U2VjcmV0");

    /* Binary data with all zero bytes */
    {
        unsigned char zeros[3] = {0, 0, 0};
        char buf[8];
        ably_base64_encode(buf, sizeof(buf), zeros, 3);
        if (strcmp(buf, "AAAA") != 0) {
            fprintf(stderr, "FAIL: zeros encode = \"%s\", want \"AAAA\"\n", buf);
            failures++;
        } else {
            printf("PASS: zeros encode = \"AAAA\"\n");
        }
    }

    if (failures == 0) {
        printf("All base64 tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
