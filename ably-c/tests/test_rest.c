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
 * Integration test for the REST client.
 * Requires: ABLY_API_KEY environment variable to be set.
 */

#include <ably/ably.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        failures++; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

int main(void)
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key) {
        printf("SKIP: ABLY_API_KEY not set — skipping REST integration tests.\n");
        return 0;
    }

    ably_set_log_level(ABLY_LOG_INFO);

    /* --- Create client --- */
    ably_rest_client_t *client = ably_rest_client_create(api_key, NULL, NULL);
    CHECK(client != NULL, "REST client create");
    if (!client) return 1;

    /* --- Single publish --- */
    ably_error_t err = ably_rest_publish(client, "ably-c-test", "unit-test", "hello");
    CHECK(err == ABLY_OK, "single publish returns ABLY_OK");
    CHECK(ably_rest_last_http_status(client) == 201, "single publish HTTP 201");

    /* --- Batch publish --- */
    ably_rest_message_t batch[3] = {
        { "evt1", "data1" },
        { "evt2", "data2" },
        { "evt3", "data3" },
    };
    err = ably_rest_publish_batch(client, "ably-c-test", batch, 3);
    CHECK(err == ABLY_OK, "batch publish returns ABLY_OK");
    CHECK(ably_rest_last_http_status(client) == 201, "batch publish HTTP 201");

    /* --- Bad API key --- */
    ably_rest_client_t *bad = ably_rest_client_create("bad.key:secret", NULL, NULL);
    CHECK(bad != NULL, "REST client with bad key creates");
    if (bad) {
        err = ably_rest_publish(bad, "test", NULL, "data");
        CHECK(err == ABLY_ERR_HTTP, "bad key publish returns ABLY_ERR_HTTP");
        CHECK(ably_rest_last_http_status(bad) == 401, "bad key HTTP 401");
        ably_rest_client_destroy(bad);
    }

    ably_rest_client_destroy(client);

    if (failures == 0) {
        printf("All REST integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
