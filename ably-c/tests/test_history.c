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
 * Integration test for REST channel history.
 *
 * Exercises:
 *   - Basic backwards/forwards history retrieval
 *   - limit parameter
 *   - Pagination via next_cursor
 *   - Correct message ordering per direction
 *   - Field integrity (name, data, timestamp)
 *   - ably_history_page_free() correctness
 *
 * Requires: ABLY_API_KEY environment variable.
 */

#include <ably/ably.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_sec(n) Sleep((DWORD)((n) * 1000))
#else
#  include <unistd.h>
#  define sleep_sec(n) sleep((unsigned)(n))
#endif

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
        g_failures++; \
    } else { \
        printf("PASS: %s\n", (msg)); \
    } \
} while (0)

#define REQUIRE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "REQUIRE FAIL [%s:%d]: %s — aborting.\n", \
                __FILE__, __LINE__, (msg)); \
        exit(1); \
    } \
} while (0)

/* Generate a unique channel name so each run has a clean slate. */
static void unique_channel(char *buf, size_t len)
{
    snprintf(buf, len, "ably-c-hist-%lld", (long long)time(NULL));
}

int main(void)
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key) {
        printf("SKIP: ABLY_API_KEY not set — skipping history integration tests.\n");
        return 0;
    }

    ably_set_log_level(ABLY_LOG_WARN);

    ably_rest_client_t *client = ably_rest_client_create(api_key, NULL, NULL);
    REQUIRE(client != NULL, "create REST client");

    char ch[128];
    unique_channel(ch, sizeof(ch));

    /* -----------------------------------------------------------------------
     * Publish 6 messages in order so we have known history content.
     * ----------------------------------------------------------------------- */
    const char *names[6] = { "e0","e1","e2","e3","e4","e5" };
    const char *datas[6] = { "d0","d1","d2","d3","d4","d5" };

    for (int i = 0; i < 6; i++) {
        ably_error_t err = ably_rest_publish(client, ch, names[i], datas[i]);
        CHECK(err == ABLY_OK, "publish for history");
    }

    /* Small delay so messages are indexed by Ably. */
    sleep_sec(2);

    /* -----------------------------------------------------------------------
     * Backwards fetch — most-recent first, no limit.
     * ----------------------------------------------------------------------- */
    printf("\n--- backwards, no limit ---\n");
    {
        ably_history_page_t *page = NULL;
        ably_error_t err = ably_rest_channel_history(client, ch, 0, "backwards", NULL, &page);
        CHECK(err == ABLY_OK, "backwards history returns ABLY_OK");
        CHECK(ably_rest_last_http_status(client) == 200, "backwards history HTTP 200");
        if (err == ABLY_OK && page) {
            CHECK(page->count >= 6, "backwards history has all 6 messages");
            /* Ably default is backwards; most recent = e5. */
            if (page->count > 0) {
                CHECK(page->items[0].name != NULL, "item[0].name non-NULL");
                CHECK(page->items[0].data != NULL, "item[0].data non-NULL");
                CHECK(page->items[0].timestamp > 0, "item[0].timestamp > 0");
                CHECK(strcmp(page->items[0].name, "e5") == 0,
                      "backwards: first item is e5 (most recent)");
                CHECK(strcmp(page->items[page->count - 1].name, "e0") == 0,
                      "backwards: last item is e0 (oldest)");
            }
            ably_history_page_free(page);
        }
    }

    /* -----------------------------------------------------------------------
     * Forwards fetch — oldest first.
     * ----------------------------------------------------------------------- */
    printf("\n--- forwards, no limit ---\n");
    {
        ably_history_page_t *page = NULL;
        ably_error_t err = ably_rest_channel_history(client, ch, 0, "forwards", NULL, &page);
        CHECK(err == ABLY_OK, "forwards history returns ABLY_OK");
        if (err == ABLY_OK && page) {
            CHECK(page->count >= 6, "forwards history has all 6 messages");
            if (page->count >= 6) {
                CHECK(strcmp(page->items[0].name, "e0") == 0,
                      "forwards: first item is e0 (oldest)");
                CHECK(strcmp(page->items[page->count - 1].name, "e5") == 0,
                      "forwards: last item is e5 (most recent)");
            }
            ably_history_page_free(page);
        }
    }

    /* -----------------------------------------------------------------------
     * Limit parameter.
     * ----------------------------------------------------------------------- */
    printf("\n--- limit=2, backwards ---\n");
    {
        ably_history_page_t *page = NULL;
        ably_error_t err = ably_rest_channel_history(client, ch, 2, "backwards", NULL, &page);
        CHECK(err == ABLY_OK, "limit=2 backwards returns ABLY_OK");
        if (err == ABLY_OK && page) {
            CHECK(page->count == 2, "limit=2 page has exactly 2 items");
            if (page->count >= 1)
                CHECK(strcmp(page->items[0].name, "e5") == 0,
                      "limit=2 backwards: first is e5");
            if (page->count >= 2)
                CHECK(strcmp(page->items[1].name, "e4") == 0,
                      "limit=2 backwards: second is e4");
            ably_history_page_free(page);
        }
    }

    /* -----------------------------------------------------------------------
     * Pagination: fetch 2 items per page, page through all 6.
     * ----------------------------------------------------------------------- */
    printf("\n--- pagination, limit=2, backwards ---\n");
    {
        int total_paginated = 0;
        char cursor[256] = "";
        int page_num = 0;

        while (1) {
            ably_history_page_t *page = NULL;
            ably_error_t err = ably_rest_channel_history(
                client, ch, 2, "backwards",
                cursor[0] ? cursor : NULL,
                &page);
            CHECK(err == ABLY_OK, "paginated history page returns ABLY_OK");
            if (err != ABLY_OK || !page) break;

            CHECK(page->count >= 1 && page->count <= 2,
                  "paginated page has 1-2 items");
            total_paginated += (int)page->count;
            page_num++;

            /* Copy cursor before freeing. */
            int has_more = page->next_cursor[0] != '\0';
            if (has_more)
                memcpy(cursor, page->next_cursor, sizeof(cursor));

            ably_history_page_free(page);

            if (!has_more || page_num >= 10) break;
        }

        CHECK(total_paginated == 6, "pagination visits all 6 messages");
        CHECK(page_num == 3, "pagination requires exactly 3 pages");
    }

    /* -----------------------------------------------------------------------
     * Empty channel — history on a channel with no messages.
     * ----------------------------------------------------------------------- */
    printf("\n--- empty channel history ---\n");
    {
        char empty_ch[128];
        snprintf(empty_ch, sizeof(empty_ch), "ably-c-hist-empty-%lld", (long long)time(NULL));

        ably_history_page_t *page = NULL;
        ably_error_t err = ably_rest_channel_history(client, empty_ch, 10, "backwards", NULL, &page);
        CHECK(err == ABLY_OK, "empty channel history returns ABLY_OK");
        if (err == ABLY_OK && page) {
            CHECK(page->count == 0, "empty channel history count is 0");
            CHECK(page->next_cursor[0] == '\0', "empty channel has no next cursor");
            ably_history_page_free(page);
        }
    }

    /* -----------------------------------------------------------------------
     * Bad credentials — expect ABLY_ERR_HTTP.
     * ----------------------------------------------------------------------- */
    printf("\n--- bad credentials ---\n");
    {
        ably_rest_client_t *bad = ably_rest_client_create("bad.key:secret", NULL, NULL);
        if (bad) {
            ably_history_page_t *page = NULL;
            ably_error_t err = ably_rest_channel_history(bad, ch, 10, NULL, NULL, &page);
            CHECK(err == ABLY_ERR_HTTP, "bad key history returns ABLY_ERR_HTTP");
            CHECK(ably_rest_last_http_status(bad) == 401, "bad key history HTTP 401");
            CHECK(page == NULL, "bad key history leaves page=NULL");
            ably_rest_client_destroy(bad);
        }
    }

    ably_rest_client_destroy(client);

    if (g_failures == 0) {
        printf("\nAll history integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) failed.\n", g_failures);
    return 1;
}
