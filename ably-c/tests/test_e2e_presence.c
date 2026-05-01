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
 * End-to-end presence test.
 *
 * Uses two realtime clients in the same process:
 *   - "observer"  — subscribes to presence events on the channel
 *   - "actor"     — enters, updates, and leaves presence
 *
 * Verifies that the observer receives the correct sequence of ENTER / UPDATE /
 * LEAVE events with matching clientId and data, and that get_members() reflects
 * the live state at each stage.
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

/* -------------------------------------------------------------------------
 * Test harness
 * ------------------------------------------------------------------------- */

static int g_failures = 0;
static int g_passes   = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
        g_failures++; \
    } else { \
        printf("PASS: %s\n", (msg)); \
        g_passes++; \
    } \
} while (0)

#define REQUIRE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FATAL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
        exit(1); \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * Shared presence event log
 * ------------------------------------------------------------------------- */

#define MAX_PRES_EVENTS 64

typedef struct {
    ably_presence_action_t action;
    char client_id[256];
    char data[256];
} pres_event_t;

typedef struct {
    volatile int count;
    pres_event_t events[MAX_PRES_EVENTS];
} pres_log_t;

static void on_presence(ably_channel_t *ch, const ably_presence_message_t *msg,
                         void *user_data)
{
    (void)ch;
    pres_log_t *log = user_data;
    int idx = log->count;
    if (idx >= MAX_PRES_EVENTS) return;
    log->events[idx].action = msg->action;
    snprintf(log->events[idx].client_id, 256, "%s", msg->client_id);
    snprintf(log->events[idx].data,      256, "%s", msg->data);
    log->count = idx + 1;
}

/* Wait until at least `expected` presence events have been logged. */
static int wait_pres(const pres_log_t *log, int expected, int max_secs)
{
    for (int i = 0; i < max_secs; i++) {
        if (log->count >= expected) return 1;
        sleep_sec(1);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Connection / channel helpers
 * ------------------------------------------------------------------------- */

static int wait_conn(ably_rt_client_t *c, ably_connection_state_t target, int secs)
{
    for (int i = 0; i < secs; i++) {
        if (ably_rt_client_state(c) == target) return 1;
        sleep_sec(1);
    }
    return 0;
}

static int wait_chan(ably_channel_t *ch, ably_channel_state_t target, int secs)
{
    for (int i = 0; i < secs; i++) {
        if (ably_channel_state(ch) == target) return 1;
        sleep_sec(1);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_enter_update_leave(const char *api_key, const char *channel_name)
{
    printf("\n=== test_enter_update_leave (channel: %s) ===\n", channel_name);

    /* ---- Set up observer ---- */

    ably_rt_client_t *obs = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(obs != NULL, "observer create");
    REQUIRE(ably_rt_client_connect(obs) == ABLY_OK, "observer connect");
    REQUIRE(wait_conn(obs, ABLY_CONN_CONNECTED, 15), "observer CONNECTED");

    ably_channel_t *obs_ch = ably_rt_channel_get(obs, channel_name);
    REQUIRE(obs_ch != NULL, "observer get channel");

    pres_log_t log = {0};
    int tok = ably_channel_presence_subscribe(obs_ch, on_presence, &log);
    CHECK(tok > 0, "observer: presence subscribe returns positive token");

    REQUIRE(ably_channel_attach(obs_ch) == ABLY_OK, "observer attach");
    REQUIRE(wait_chan(obs_ch, ABLY_CHAN_ATTACHED, 10), "observer ATTACHED");

    /* ---- Set up actor ---- */

    ably_rt_client_t *actor = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(actor != NULL, "actor create");
    REQUIRE(ably_rt_client_connect(actor) == ABLY_OK, "actor connect");
    REQUIRE(wait_conn(actor, ABLY_CONN_CONNECTED, 15), "actor CONNECTED");

    ably_channel_t *actor_ch = ably_rt_channel_get(actor, channel_name);
    REQUIRE(actor_ch != NULL, "actor get channel");
    REQUIRE(ably_channel_attach(actor_ch) == ABLY_OK, "actor attach");
    REQUIRE(wait_chan(actor_ch, ABLY_CHAN_ATTACHED, 10), "actor ATTACHED");

    /* ---- ENTER ---- */

    ably_error_t err = ably_channel_presence_enter(actor_ch, "actor-1", "data-enter");
    CHECK(err == ABLY_OK, "actor: presence_enter returns ABLY_OK");

    int got = wait_pres(&log, 1, 12);
    CHECK(got, "observer received ENTER event within 12s");
    if (got && log.count >= 1) {
        CHECK(log.events[0].action == ABLY_PRESENCE_ENTER,
              "ENTER event has action=ENTER");
        CHECK(strcmp(log.events[0].client_id, "actor-1") == 0,
              "ENTER event has correct clientId");
        CHECK(strcmp(log.events[0].data, "data-enter") == 0,
              "ENTER event has correct data");
    }

    /* Verify get_members after enter. */
    ably_presence_message_t members[32];
    int total = 0;
    int n = ably_channel_presence_get_members(obs_ch, members, 32, &total);
    CHECK(total >= 1, "get_members: total >= 1 after ENTER");
    {
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(members[i].client_id, "actor-1") == 0) { found = 1; break; }
        }
        CHECK(found, "get_members: actor-1 present after ENTER");
    }

    /* ---- UPDATE ---- */

    int before_update = log.count;
    err = ably_channel_presence_update(actor_ch, "data-updated");
    CHECK(err == ABLY_OK, "actor: presence_update returns ABLY_OK");

    got = wait_pres(&log, before_update + 1, 12);
    CHECK(got, "observer received UPDATE event within 12s");
    if (got && log.count > before_update) {
        int idx = before_update;
        CHECK(log.events[idx].action == ABLY_PRESENCE_UPDATE,
              "UPDATE event has action=UPDATE");
        CHECK(strcmp(log.events[idx].client_id, "actor-1") == 0,
              "UPDATE event has correct clientId");
        CHECK(strcmp(log.events[idx].data, "data-updated") == 0,
              "UPDATE event has correct data");
    }

    /* get_members should reflect updated data. */
    total = 0;
    n = ably_channel_presence_get_members(obs_ch, members, 32, &total);
    {
        int found_updated = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(members[i].client_id, "actor-1") == 0 &&
                strcmp(members[i].data,      "data-updated") == 0)
            {
                found_updated = 1;
                break;
            }
        }
        CHECK(found_updated, "get_members: actor-1 data updated");
    }

    /* ---- LEAVE ---- */

    int before_leave = log.count;
    err = ably_channel_presence_leave(actor_ch, "data-leave");
    CHECK(err == ABLY_OK, "actor: presence_leave returns ABLY_OK");

    got = wait_pres(&log, before_leave + 1, 12);
    CHECK(got, "observer received LEAVE event within 12s");
    if (got && log.count > before_leave) {
        int idx = before_leave;
        CHECK(log.events[idx].action == ABLY_PRESENCE_LEAVE,
              "LEAVE event has action=LEAVE");
        CHECK(strcmp(log.events[idx].client_id, "actor-1") == 0,
              "LEAVE event has correct clientId");
    }

    /* get_members should not contain actor after LEAVE. */
    total = 0;
    n = ably_channel_presence_get_members(obs_ch, members, 32, &total);
    {
        int still_there = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(members[i].client_id, "actor-1") == 0) { still_there = 1; break; }
        }
        CHECK(!still_there, "get_members: actor-1 absent after LEAVE");
    }

    /* Unsubscribe observer. */
    ably_channel_presence_unsubscribe(obs_ch, tok);

    ably_rt_client_close(actor, 5000);
    ably_rt_client_destroy(actor);
    ably_rt_client_close(obs, 5000);
    ably_rt_client_destroy(obs);
}

static void test_multiple_members(const char *api_key, const char *channel_name)
{
    printf("\n=== test_multiple_members (channel: %s) ===\n", channel_name);

    /* Observer. */
    ably_rt_client_t *obs = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(obs != NULL, "observer create");
    REQUIRE(ably_rt_client_connect(obs) == ABLY_OK, "observer connect");
    REQUIRE(wait_conn(obs, ABLY_CONN_CONNECTED, 15), "observer CONNECTED");
    ably_channel_t *obs_ch = ably_rt_channel_get(obs, channel_name);
    REQUIRE(obs_ch != NULL, "observer get channel");

    pres_log_t log = {0};
    ably_channel_presence_subscribe(obs_ch, on_presence, &log);
    REQUIRE(ably_channel_attach(obs_ch) == ABLY_OK, "observer attach");
    REQUIRE(wait_chan(obs_ch, ABLY_CHAN_ATTACHED, 10), "observer ATTACHED");

    /* Actor A. */
    ably_rt_client_t *a = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(a != NULL, "actor-A create");
    REQUIRE(ably_rt_client_connect(a) == ABLY_OK, "actor-A connect");
    REQUIRE(wait_conn(a, ABLY_CONN_CONNECTED, 15), "actor-A CONNECTED");
    ably_channel_t *a_ch = ably_rt_channel_get(a, channel_name);
    REQUIRE(a_ch != NULL, "actor-A channel");
    REQUIRE(ably_channel_attach(a_ch) == ABLY_OK, "actor-A attach");
    REQUIRE(wait_chan(a_ch, ABLY_CHAN_ATTACHED, 10), "actor-A ATTACHED");

    /* Actor B. */
    ably_rt_client_t *b = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(b != NULL, "actor-B create");
    REQUIRE(ably_rt_client_connect(b) == ABLY_OK, "actor-B connect");
    REQUIRE(wait_conn(b, ABLY_CONN_CONNECTED, 15), "actor-B CONNECTED");
    ably_channel_t *b_ch = ably_rt_channel_get(b, channel_name);
    REQUIRE(b_ch != NULL, "actor-B channel");
    REQUIRE(ably_channel_attach(b_ch) == ABLY_OK, "actor-B attach");
    REQUIRE(wait_chan(b_ch, ABLY_CHAN_ATTACHED, 10), "actor-B ATTACHED");

    /* Both enter presence. */
    REQUIRE(ably_channel_presence_enter(a_ch, "member-a", "da") == ABLY_OK, "A enter");
    REQUIRE(ably_channel_presence_enter(b_ch, "member-b", "db") == ABLY_OK, "B enter");

    /* Wait for both ENTERs at the observer. */
    int got = wait_pres(&log, 2, 15);
    CHECK(got, "observer received 2 ENTER events within 15s");

    /* get_members should now report both. */
    ably_presence_message_t members[32];
    int total = 0;
    int n = ably_channel_presence_get_members(obs_ch, members, 32, &total);
    CHECK(total >= 2, "get_members: >= 2 members after both enter");

    int found_a = 0, found_b = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(members[i].client_id, "member-a") == 0) found_a = 1;
        if (strcmp(members[i].client_id, "member-b") == 0) found_b = 1;
    }
    CHECK(found_a, "get_members: member-a present");
    CHECK(found_b, "get_members: member-b present");

    /* A leaves; observer should see LEAVE and member-a drops from map. */
    int before = log.count;
    REQUIRE(ably_channel_presence_leave(a_ch, "") == ABLY_OK, "A leave");
    got = wait_pres(&log, before + 1, 12);
    CHECK(got, "observer received LEAVE for A within 12s");

    total = 0;
    n = ably_channel_presence_get_members(obs_ch, members, 32, &total);
    found_a = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(members[i].client_id, "member-a") == 0) found_a = 1;
    }
    CHECK(!found_a, "member-a absent after leave");
    CHECK(total >= 1, "member-b still present");

    ably_rt_client_close(b, 5000);
    ably_rt_client_destroy(b);
    ably_rt_client_close(a, 5000);
    ably_rt_client_destroy(a);
    ably_rt_client_close(obs, 5000);
    ably_rt_client_destroy(obs);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int main(void)
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key || !*api_key) {
        printf("SKIP: ABLY_API_KEY not set — skipping presence e2e tests.\n");
        return 77;   /* ctest treats 77 as SKIP */
    }

    ably_set_log_level(ABLY_LOG_WARN);

    /* Use a timestamped channel name to avoid cross-run interference. */
    char ch1[64], ch2[64];
    long ts = (long)time(NULL);
    snprintf(ch1, sizeof(ch1), "ably-c-pres-e2e-1-%ld", ts);
    snprintf(ch2, sizeof(ch2), "ably-c-pres-e2e-2-%ld", ts);

    test_enter_update_leave(api_key, ch1);
    test_multiple_members(api_key, ch2);

    printf("\n=== Results: %d passed, %d failed ===\n", g_passes, g_failures);

    return g_failures == 0 ? 0 : 1;
}
