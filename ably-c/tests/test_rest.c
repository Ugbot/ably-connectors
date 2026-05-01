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

    /* --- Channel history --- */
    {
        const char *hist_ch = "ably-c-history-test";

        /* Publish known messages so history has content. */
        err = ably_rest_publish(client, hist_ch, "h1", "payload-1");
        CHECK(err == ABLY_OK, "history: publish msg1");
        err = ably_rest_publish(client, hist_ch, "h2", "payload-2");
        CHECK(err == ABLY_OK, "history: publish msg2");
        err = ably_rest_publish(client, hist_ch, "h3", "payload-3");
        CHECK(err == ABLY_OK, "history: publish msg3");

        ably_history_page_t *page = NULL;
        err = ably_rest_channel_history(client, hist_ch,
                                         10, "backwards", NULL, &page);
        CHECK(err == ABLY_OK, "history fetch returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 200, "history HTTP 200");
        if (err == ABLY_OK && page) {
            CHECK(page->count >= 3, "history page has at least 3 messages");
            /* Backwards direction: most-recent first. */
            if (page->count > 0) {
                CHECK(page->items[0].name != NULL, "history item[0] has name");
                CHECK(page->items[0].data != NULL, "history item[0] has data");
            }
            ably_history_page_free(page);
        }

        /* Limit=1 — should only return 1 item. */
        page = NULL;
        err = ably_rest_channel_history(client, hist_ch, 1, "backwards", NULL, &page);
        CHECK(err == ABLY_OK, "history fetch limit=1 returns ABLY_OK");
        if (err == ABLY_OK && page) {
            CHECK(page->count == 1, "history with limit=1 returns exactly 1 item");
            ably_history_page_free(page);
        }

        /* Bad API key → 401. */
        if (bad) {
            page = NULL;
            err = ably_rest_channel_history(bad, hist_ch, 0, NULL, NULL, &page);
            CHECK(err == ABLY_ERR_HTTP, "history bad key returns ABLY_ERR_HTTP");
            CHECK(page == NULL, "history bad key sets page=NULL");
        }
    }

    /* --- Server time --- */
    {
        int64_t server_time = 0;
        err = ably_rest_time(client, &server_time);
        CHECK(err == ABLY_OK, "rest_time returns ABLY_OK");
        /* Ably server time should be a reasonable Unix ms timestamp (after 2020). */
        CHECK(server_time > (int64_t)1577836800000LL, "server time is after 2020");
    }

    /* --- Channel status --- */
    {
        /* Publish to ensure the channel is active. */
        const char *stat_ch = "ably-c-status-test";
        err = ably_rest_publish(client, stat_ch, "ping", "pong");
        CHECK(err == ABLY_OK, "status: publish to activate channel");

        ably_channel_status_t status;
        memset(&status, 0, sizeof(status));
        err = ably_rest_channel_status(client, stat_ch, &status);
        CHECK(err == ABLY_OK, "channel status returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 200, "channel status HTTP 200");
        if (err == ABLY_OK) {
            CHECK(strcmp(status.name, stat_ch) == 0,
                  "channel status name matches");
            /* Connections/subscribers may be 0 if no realtime subscribers. */
            CHECK(status.occupancy.connections >= 0, "connections non-negative");
            CHECK(status.occupancy.publishers  >= 0, "publishers non-negative");
        }

        /* Non-existent channel returns 200 with zero occupancy (Ably behaviour). */
        ably_channel_status_t empty_status;
        memset(&empty_status, 0, sizeof(empty_status));
        err = ably_rest_channel_status(client, "ably-c-nonexistent-xyz-404", &empty_status);
        /* Ably returns 200 for non-existent channels too, with is_active=0. */
        CHECK(err == ABLY_OK || err == ABLY_ERR_HTTP,
              "nonexistent channel status does not crash");
    }

    ably_rest_client_destroy(client);

    if (failures == 0) {
        printf("All REST integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
