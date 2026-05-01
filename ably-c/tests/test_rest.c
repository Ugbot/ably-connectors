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
        { "evt1", "data1", NULL },
        { "evt2", "data2", NULL },
        { "evt3", "data3", NULL },
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

    /* --- Stats --- */
    {
        ably_stats_page_t *stats_page = NULL;
        /* Fetch last hour of stats at minute granularity, limit to 5. */
        err = ably_rest_stats(client, "minute", 0, 0, "backwards", 5, &stats_page);
        CHECK(err == ABLY_OK, "rest_stats returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 200, "rest_stats HTTP 200");
        if (err == ABLY_OK && stats_page) {
            CHECK(stats_page->count <= 5, "stats page count <= limit");
            /* interval_id should be a non-empty string on any active Ably account. */
            if (stats_page->count > 0) {
                CHECK(stats_page->items[0].interval_id[0] != '\0',
                      "stats[0].interval_id non-empty");
                CHECK(stats_page->items[0].unit[0] != '\0',
                      "stats[0].unit non-empty");
            }
            ably_stats_page_free(stats_page);
        }

        /* day granularity */
        stats_page = NULL;
        err = ably_rest_stats(client, "day", 0, 0, "backwards", 1, &stats_page);
        CHECK(err == ABLY_OK, "rest_stats day granularity returns ABLY_OK");
        if (err == ABLY_OK && stats_page) {
            ably_stats_page_free(stats_page);
        }

        /* Bad API key → 401. */
        stats_page = NULL;
        ably_rest_client_t *bad_stats = ably_rest_client_create("bad.key:secret", NULL, NULL);
        if (bad_stats) {
            err = ably_rest_stats(bad_stats, NULL, 0, 0, NULL, 0, &stats_page);
            CHECK(err == ABLY_ERR_HTTP, "stats bad key returns ABLY_ERR_HTTP");
            CHECK(stats_page == NULL, "stats bad key page is NULL");
            ably_rest_client_destroy(bad_stats);
        }
    }

    /* --- Generic REST request --- */
    {
        /* GET /time via generic request. */
        ably_rest_response_t resp;
        memset(&resp, 0, sizeof(resp));
        err = ably_rest_request(client, "GET", "/time", NULL, 0, &resp);
        CHECK(err == ABLY_OK, "rest_request GET /time returns ABLY_OK");
        CHECK(resp.http_status == 200, "rest_request GET /time HTTP 200");
        CHECK(resp.body != NULL && resp.body_len > 0, "rest_request GET /time has body");

        /* POST a message via generic request. */
        const char *req_body = "{\"name\":\"req-test\",\"data\":\"req-data\"}";
        memset(&resp, 0, sizeof(resp));
        err = ably_rest_request(client, "POST",
                                 "/channels/ably-c-request-test/messages",
                                 req_body, strlen(req_body), &resp);
        CHECK(err == ABLY_OK, "rest_request POST message returns ABLY_OK");
        CHECK(resp.http_status == 201, "rest_request POST message HTTP 201");
    }

    /* --- Multi-channel batch publish --- */
    {
        ably_rest_message_t msgs_a[2] = {
            { "batch-evt", "val-1", NULL },
            { "batch-evt", "val-2", NULL },
        };
        ably_rest_message_t msgs_b[1] = {
            { "batch-evt", "val-3", NULL },
        };
        ably_rest_batch_spec_t specs[2] = {
            { "ably-c-batch-a", msgs_a, 2 },
            { "ably-c-batch-b", msgs_b, 1 },
        };
        ably_rest_batch_result_t results[4];
        size_t result_count = 0;
        err = ably_rest_batch_publish(client, specs, 2,
                                       results, 4, &result_count);
        CHECK(err == ABLY_OK, "multi-channel batch publish returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 201,
              "multi-channel batch publish HTTP 201");
    }

    /* --- Channel list --- */
    {
        /* Publish to ensure at least one ably-c-* channel is active. */
        err = ably_rest_publish(client, "ably-c-list-probe", "probe", "1");
        CHECK(err == ABLY_OK, "channel_list: probe publish OK");

        ably_channel_list_page_t *list_page = NULL;
        err = ably_rest_channel_list(client, "ably-c-", 10, &list_page);
        CHECK(err == ABLY_OK, "channel_list with prefix returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 200, "channel_list HTTP 200");
        if (err == ABLY_OK && list_page) {
            CHECK(list_page->count >= 0, "channel_list count non-negative");
            ably_channel_list_page_free(list_page);
        }

        /* NULL prefix — list all (may be > 0 channels). */
        list_page = NULL;
        err = ably_rest_channel_list(client, NULL, 5, &list_page);
        CHECK(err == ABLY_OK, "channel_list no prefix returns ABLY_OK");
        if (err == ABLY_OK && list_page) {
            CHECK(list_page->count <= 5, "channel_list respects limit=5");
            ably_channel_list_page_free(list_page);
        }
    }

    /* --- Token request (requestToken) --- */
    {
        ably_token_params_t tparams;
        memset(&tparams, 0, sizeof(tparams));
        tparams.capability = "{\"*\":[\"*\"]}";
        tparams.ttl_ms     = 60000; /* 1 minute */

        ably_token_details_t tdetails;
        memset(&tdetails, 0, sizeof(tdetails));
        err = ably_rest_request_token(client, &tparams, &tdetails);
        CHECK(err == ABLY_OK, "request_token returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 200, "request_token HTTP 200");
        if (err == ABLY_OK) {
            CHECK(tdetails.token[0] != '\0', "token string non-empty");
            CHECK(tdetails.issued > 0, "token issued timestamp > 0");
            CHECK(tdetails.expires > tdetails.issued, "token expires after issued");
        }

        /* NULL params — server defaults. */
        memset(&tdetails, 0, sizeof(tdetails));
        err = ably_rest_request_token(client, NULL, &tdetails);
        CHECK(err == ABLY_OK, "request_token NULL params returns ABLY_OK");
        if (err == ABLY_OK) {
            CHECK(tdetails.token[0] != '\0', "request_token NULL params: token non-empty");
        }

        /* Use the issued token to create a Bearer-auth client and publish. */
        if (tdetails.token[0] != '\0') {
            ably_rest_options_t topts;
            ably_rest_options_init(&topts);
            topts.token = tdetails.token;
            ably_rest_client_t *token_client = ably_rest_client_create("dummy:dummy", &topts, NULL);
            CHECK(token_client != NULL, "token-auth client creates");
            if (token_client) {
                err = ably_rest_publish(token_client, "ably-c-token-test", "tok-evt", "tok-data");
                CHECK(err == ABLY_OK, "token-auth client publish returns ABLY_OK");
                CHECK(ably_rest_last_http_status(token_client) == 201,
                      "token-auth client publish HTTP 201");
                ably_rest_client_destroy(token_client);
            }
        }
    }

    /* --- REST presence.get() --- */
    {
        /* No realtime subscribers → count may be 0; just verify no crash. */
        ably_presence_page_t *pres_page = NULL;
        err = ably_rest_presence_get(client, "ably-c-pres-rest", 10, NULL, &pres_page);
        CHECK(err == ABLY_OK || err == ABLY_ERR_HTTP,
              "rest_presence_get returns OK or error");
        if (err == ABLY_OK && pres_page) {
            CHECK(pres_page->count >= 0, "presence page count non-negative");
            ably_presence_page_free(pres_page);
        }

        /* clientId filter on empty channel. */
        pres_page = NULL;
        err = ably_rest_presence_get(client, "ably-c-pres-rest", 5, "nobody", &pres_page);
        CHECK(err == ABLY_OK || err == ABLY_ERR_HTTP,
              "rest_presence_get with clientId filter doesn't crash");
        if (err == ABLY_OK && pres_page) {
            ably_presence_page_free(pres_page);
        }
    }

    ably_rest_client_destroy(client);

    if (failures == 0) {
        printf("All REST integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
