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
 * Integration test for the real-time client.
 *
 * Tests:
 *   1. Connect and reach CONNECTED state.
 *   2. Attach a channel and reach ATTACHED state.
 *   3. Subscribe and receive a message published via the REST client.
 *   4. Unsubscribe (message should no longer be delivered).
 *   5. Detach channel, clean close.
 *
 * Requires: ABLY_API_KEY environment variable.
 */

#include <ably/ably.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_sec(n) Sleep((DWORD)((n) * 1000))
#else
#  include <unistd.h>
#  define sleep_sec(n) sleep((unsigned)(n))
#endif

/* ---- Minimal test harness ---- */

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

/* ---- Shared callback state ---- */

typedef struct {
    volatile int     received;
    char             last_name[256];
    char             last_data[256];
} msg_state_t;

static void on_message(ably_channel_t       *channel,
                        const ably_message_t *msg,
                        void                 *user_data)
{
    (void)channel;
    msg_state_t *s = user_data;
    if (msg->name) snprintf(s->last_name, sizeof(s->last_name), "%s", msg->name);
    if (msg->data) snprintf(s->last_data, sizeof(s->last_data), "%s", msg->data);
    s->received++;
}

/* ---- Wait helpers ---- */

static int wait_for_conn(ably_rt_client_t *client,
                          ably_connection_state_t target, int max_secs)
{
    for (int i = 0; i < max_secs; i++) {
        if (ably_rt_client_state(client) == target) return 1;
        sleep_sec(1);
    }
    return 0;
}

static int wait_for_chan(ably_channel_t *channel,
                          ably_channel_state_t target, int max_secs)
{
    for (int i = 0; i < max_secs; i++) {
        if (ably_channel_state(channel) == target) return 1;
        sleep_sec(1);
    }
    return 0;
}

static int wait_for_msg(const msg_state_t *s, int expected, int max_secs)
{
    for (int i = 0; i < max_secs; i++) {
        if (s->received >= expected) return 1;
        sleep_sec(1);
    }
    return 0;
}

/* ---- Tests ---- */

static void test_connect_and_close(const char *api_key)
{
    printf("\n--- test_connect_and_close ---\n");

    ably_rt_client_t *client = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(client != NULL, "create realtime client");

    ably_error_t err = ably_rt_client_connect(client);
    CHECK(err == ABLY_OK, "connect returns ABLY_OK");

    int connected = wait_for_conn(client, ABLY_CONN_CONNECTED, 15);
    CHECK(connected, "reach CONNECTED within 15s");

    err = ably_rt_client_close(client, 5000);
    CHECK(err == ABLY_OK, "close returns ABLY_OK");
    CHECK(ably_rt_client_state(client) == ABLY_CONN_CLOSED, "state is CLOSED after close");

    ably_rt_client_destroy(client);
}

static void test_channel_attach(const char *api_key)
{
    printf("\n--- test_channel_attach ---\n");

    ably_rt_client_t *client = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(client != NULL, "create realtime client");

    ably_error_t err = ably_rt_client_connect(client);
    REQUIRE(err == ABLY_OK, "connect");
    REQUIRE(wait_for_conn(client, ABLY_CONN_CONNECTED, 15), "reach CONNECTED");

    ably_channel_t *channel = ably_rt_channel_get(client, "ably-c-integration");
    REQUIRE(channel != NULL, "get channel");

    err = ably_channel_attach(channel);
    CHECK(err == ABLY_OK, "attach returns ABLY_OK");

    int attached = wait_for_chan(channel, ABLY_CHAN_ATTACHED, 10);
    CHECK(attached, "reach ATTACHED within 10s");

    CHECK(strcmp(ably_channel_name(channel), "ably-c-integration") == 0,
          "channel name matches");

    err = ably_channel_detach(channel);
    CHECK(err == ABLY_OK, "detach returns ABLY_OK");

    ably_rt_client_close(client, 5000);
    ably_rt_client_destroy(client);
}

static void test_subscribe_receive(const char *api_key)
{
    printf("\n--- test_subscribe_receive ---\n");

    static const char *channel_name = "ably-c-integration";

    /* Real-time subscriber. */
    ably_rt_client_t *rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(rt != NULL, "create realtime client");

    ably_error_t err = ably_rt_client_connect(rt);
    REQUIRE(err == ABLY_OK, "connect realtime");
    REQUIRE(wait_for_conn(rt, ABLY_CONN_CONNECTED, 15), "realtime CONNECTED");

    ably_channel_t *channel = ably_rt_channel_get(rt, channel_name);
    REQUIRE(channel != NULL, "get channel");

    msg_state_t state = {0};
    int token = ably_channel_subscribe(channel, "rt-test", on_message, &state);
    CHECK(token > 0, "subscribe returns positive token");

    err = ably_channel_attach(channel);
    REQUIRE(err == ABLY_OK, "attach");
    REQUIRE(wait_for_chan(channel, ABLY_CHAN_ATTACHED, 10), "channel ATTACHED");

    /* Publish via REST. */
    ably_rest_client_t *rest = ably_rest_client_create(api_key, NULL, NULL);
    REQUIRE(rest != NULL, "create REST client");

    err = ably_rest_publish(rest, channel_name, "rt-test", "hello-realtime");
    CHECK(err == ABLY_OK, "REST publish to realtime channel");

    /* Wait for the message to arrive. */
    int got = wait_for_msg(&state, 1, 10);
    CHECK(got, "message received within 10s");
    if (got) {
        CHECK(strcmp(state.last_name, "rt-test") == 0, "message name matches");
        CHECK(strcmp(state.last_data, "hello-realtime") == 0, "message data matches");
    }

    /* Unsubscribe — subsequent messages must not arrive. */
    err = ably_channel_unsubscribe(channel, token);
    CHECK(err == ABLY_OK, "unsubscribe returns ABLY_OK");

    int before = state.received;
    ably_rest_publish(rest, channel_name, "rt-test", "should-not-arrive");
    sleep_sec(3);
    CHECK(state.received == before, "no message delivered after unsubscribe");

    ably_rest_client_destroy(rest);
    ably_rt_client_close(rt, 5000);
    ably_rt_client_destroy(rt);
}

static void test_realtime_publish(const char *api_key)
{
    printf("\n--- test_realtime_publish ---\n");

    static const char *channel_name = "ably-c-rt-publish";

    ably_rt_client_t *rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(rt != NULL, "create realtime client");

    ably_error_t err = ably_rt_client_connect(rt);
    REQUIRE(err == ABLY_OK, "connect");
    REQUIRE(wait_for_conn(rt, ABLY_CONN_CONNECTED, 15), "CONNECTED");

    ably_channel_t *channel = ably_rt_channel_get(rt, channel_name);
    REQUIRE(channel != NULL, "get channel");

    msg_state_t state = {0};
    ably_channel_subscribe(channel, "echo", on_message, &state);

    err = ably_channel_attach(channel);
    REQUIRE(err == ABLY_OK, "attach");
    REQUIRE(wait_for_chan(channel, ABLY_CHAN_ATTACHED, 10), "ATTACHED");

    /* Publish via the real-time connection itself. */
    err = ably_channel_publish(channel, "echo", "ping");
    CHECK(err == ABLY_OK, "realtime publish returns ABLY_OK");

    /* Ably echo semantics: messages published on the real-time connection
     * are echoed back unless echo is disabled.  Default: echo enabled. */
    int got = wait_for_msg(&state, 1, 10);
    CHECK(got, "echo message received within 10s");
    if (got) {
        CHECK(strcmp(state.last_data, "ping") == 0, "echo data matches");
    }

    /* Reject publish when not ATTACHED. */
    err = ably_channel_detach(channel);
    CHECK(err == ABLY_OK, "detach");
    sleep_sec(2);
    err = ably_channel_publish(channel, "echo", "should-fail");
    CHECK(err == ABLY_ERR_STATE, "publish on detached channel returns ABLY_ERR_STATE");

    ably_rt_client_close(rt, 5000);
    ably_rt_client_destroy(rt);
}

static void test_name_filter(const char *api_key)
{
    printf("\n--- test_name_filter ---\n");

    static const char *channel_name = "ably-c-filter-test";

    ably_rt_client_t *rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(rt != NULL, "create realtime client");

    ably_error_t err = ably_rt_client_connect(rt);
    REQUIRE(err == ABLY_OK, "connect");
    REQUIRE(wait_for_conn(rt, ABLY_CONN_CONNECTED, 15), "CONNECTED");

    ably_channel_t *channel = ably_rt_channel_get(rt, channel_name);
    REQUIRE(channel != NULL, "get channel");

    msg_state_t filtered = {0};
    msg_state_t catchall = {0};

    /* Subscribe with a name filter. */
    int tok1 = ably_channel_subscribe(channel, "target-event", on_message, &filtered);
    int tok2 = ably_channel_subscribe(channel, NULL,           on_message, &catchall);
    CHECK(tok1 > 0, "filtered subscribe token");
    CHECK(tok2 > 0, "catchall subscribe token");
    (void)tok2;

    err = ably_channel_attach(channel);
    REQUIRE(err == ABLY_OK, "attach");
    REQUIRE(wait_for_chan(channel, ABLY_CHAN_ATTACHED, 10), "ATTACHED");

    ably_rest_client_t *rest = ably_rest_client_create(api_key, NULL, NULL);
    REQUIRE(rest != NULL, "create REST client");

    ably_rest_publish(rest, channel_name, "other-event",  "data-A");
    ably_rest_publish(rest, channel_name, "target-event", "data-B");

    sleep_sec(5);

    CHECK(catchall.received >= 2, "catchall receives both messages");
    CHECK(filtered.received == 1, "filtered subscriber receives only target-event");
    if (filtered.received == 1) {
        CHECK(strcmp(filtered.last_data, "data-B") == 0,
              "filtered subscriber receives correct message");
    }

    ably_rest_client_destroy(rest);
    ably_rt_client_close(rt, 5000);
    ably_rt_client_destroy(rt);
}

/* ---- Presence integration tests ---- */

typedef struct {
    volatile int count;
    ably_presence_action_t last_action;
    char last_client_id[256];
    char last_data[256];
} pres_state_t;

static void on_presence(ably_channel_t *ch, const ably_presence_message_t *msg,
                         void *user_data)
{
    (void)ch;
    pres_state_t *s = user_data;
    s->last_action = msg->action;
    snprintf(s->last_client_id, sizeof(s->last_client_id), "%s", msg->client_id);
    snprintf(s->last_data,      sizeof(s->last_data),      "%s", msg->data);
    s->count++;
}

static int wait_for_presence(const pres_state_t *s, int expected, int max_secs)
{
    for (int i = 0; i < max_secs; i++) {
        if (s->count >= expected) return 1;
        sleep_sec(1);
    }
    return 0;
}

static void test_presence_enter_and_get(const char *api_key)
{
    printf("\n--- test_presence_enter_and_get ---\n");

    static const char *channel_name = "ably-c-presence-test";

    ably_rt_client_t *rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(rt != NULL, "create realtime client");

    ably_error_t err = ably_rt_client_connect(rt);
    REQUIRE(err == ABLY_OK, "connect");
    REQUIRE(wait_for_conn(rt, ABLY_CONN_CONNECTED, 15), "CONNECTED");

    ably_channel_t *channel = ably_rt_channel_get(rt, channel_name);
    REQUIRE(channel != NULL, "get channel");

    err = ably_channel_attach(channel);
    REQUIRE(err == ABLY_OK, "attach");
    REQUIRE(wait_for_chan(channel, ABLY_CHAN_ATTACHED, 10), "ATTACHED");

    /* Enter presence. */
    err = ably_channel_presence_enter(channel, "test-client-1", "my-data");
    CHECK(err == ABLY_OK, "presence enter returns ABLY_OK");

    /* Wait for server to ACK the enter — give the echo time to arrive. */
    sleep_sec(3);

    /* get_members should include our own entry. */
    ably_presence_message_t members[32];
    int total = 0;
    int written = ably_channel_presence_get_members(channel, members, 32, &total);
    CHECK(total >= 1, "at least 1 presence member after enter");
    CHECK(written >= 1, "get_members returns >= 1 entry");
    if (written >= 1) {
        int found = 0;
        for (int i = 0; i < written; i++) {
            if (strcmp(members[i].client_id, "test-client-1") == 0) found = 1;
        }
        CHECK(found, "own clientId found in presence members");
    }

    /* Update presence data. */
    err = ably_channel_presence_update(channel, "updated-data");
    CHECK(err == ABLY_OK, "presence update returns ABLY_OK");
    sleep_sec(2);

    /* Leave presence. */
    err = ably_channel_presence_leave(channel, "goodbye");
    CHECK(err == ABLY_OK, "presence leave returns ABLY_OK");
    sleep_sec(2);

    total = 0;
    written = ably_channel_presence_get_members(channel, members, 32, &total);
    int still_present = 0;
    for (int i = 0; i < written; i++) {
        if (strcmp(members[i].client_id, "test-client-1") == 0) still_present = 1;
    }
    CHECK(!still_present, "clientId removed from presence after leave");

    ably_rt_client_close(rt, 5000);
    ably_rt_client_destroy(rt);
}

static void test_presence_subscribe(const char *api_key)
{
    printf("\n--- test_presence_subscribe ---\n");

    static const char *channel_name = "ably-c-pres-subscribe-test";

    /* Subscriber client. */
    ably_rt_client_t *sub_rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(sub_rt != NULL, "create subscriber client");
    REQUIRE(ably_rt_client_connect(sub_rt) == ABLY_OK, "subscriber connect");
    REQUIRE(wait_for_conn(sub_rt, ABLY_CONN_CONNECTED, 15), "subscriber CONNECTED");

    ably_channel_t *sub_ch = ably_rt_channel_get(sub_rt, channel_name);
    REQUIRE(sub_ch != NULL, "subscriber get channel");

    pres_state_t pres = {0};
    int tok = ably_channel_presence_subscribe(sub_ch, on_presence, &pres);
    CHECK(tok > 0, "presence subscribe returns positive token");

    REQUIRE(ably_channel_attach(sub_ch) == ABLY_OK, "subscriber attach");
    REQUIRE(wait_for_chan(sub_ch, ABLY_CHAN_ATTACHED, 10), "subscriber ATTACHED");

    /* Entering client. */
    ably_rt_client_t *enter_rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(enter_rt != NULL, "create entering client");
    REQUIRE(ably_rt_client_connect(enter_rt) == ABLY_OK, "entering client connect");
    REQUIRE(wait_for_conn(enter_rt, ABLY_CONN_CONNECTED, 15), "entering client CONNECTED");

    ably_channel_t *enter_ch = ably_rt_channel_get(enter_rt, channel_name);
    REQUIRE(enter_ch != NULL, "entering client get channel");
    REQUIRE(ably_channel_attach(enter_ch) == ABLY_OK, "entering client attach");
    REQUIRE(wait_for_chan(enter_ch, ABLY_CHAN_ATTACHED, 10), "entering client ATTACHED");

    ably_error_t err = ably_channel_presence_enter(enter_ch, "entering-client", "hello");
    CHECK(err == ABLY_OK, "entering client: presence enter OK");

    int got_enter = wait_for_presence(&pres, 1, 10);
    CHECK(got_enter, "subscriber received ENTER event within 10s");
    if (got_enter) {
        CHECK(pres.last_action == ABLY_PRESENCE_ENTER, "subscriber saw ENTER action");
        CHECK(strcmp(pres.last_client_id, "entering-client") == 0,
              "subscriber saw correct clientId");
        CHECK(strcmp(pres.last_data, "hello") == 0, "subscriber saw correct data");
    }

    /* Leave — subscriber should receive LEAVE. */
    int before = pres.count;
    err = ably_channel_presence_leave(enter_ch, "bye");
    CHECK(err == ABLY_OK, "entering client: presence leave OK");

    int got_leave = wait_for_presence(&pres, before + 1, 10);
    CHECK(got_leave, "subscriber received LEAVE event within 10s");
    if (got_leave) {
        CHECK(pres.last_action == ABLY_PRESENCE_LEAVE, "subscriber saw LEAVE action");
    }

    ably_channel_presence_unsubscribe(sub_ch, tok);

    ably_rt_client_close(enter_rt, 5000);
    ably_rt_client_destroy(enter_rt);
    ably_rt_client_close(sub_rt, 5000);
    ably_rt_client_destroy(sub_rt);
}

static void test_rewind(const char *api_key)
{
    printf("\n--- test_rewind ---\n");

    static const char *channel_name = "ably-c-rewind-test";

    /* Publish a known message first via REST. */
    ably_rest_client_t *rest = ably_rest_client_create(api_key, NULL, NULL);
    REQUIRE(rest != NULL, "create REST client for rewind");
    ably_error_t err = ably_rest_publish(rest, channel_name, "rewind-event", "rewind-payload");
    CHECK(err == ABLY_OK, "REST publish for rewind test");
    ably_rest_client_destroy(rest);

    /* Small delay so the message is in history. */
    sleep_sec(1);

    ably_rt_client_t *rt = ably_rt_client_create(api_key, NULL, NULL);
    REQUIRE(rt != NULL, "create realtime client for rewind");
    REQUIRE(ably_rt_client_connect(rt) == ABLY_OK, "connect");
    REQUIRE(wait_for_conn(rt, ABLY_CONN_CONNECTED, 15), "CONNECTED");

    ably_channel_t *channel = ably_rt_channel_get(rt, channel_name);
    REQUIRE(channel != NULL, "get channel");

    /* Request rewind of 1 message. */
    ably_channel_set_rewind(channel, 1);

    msg_state_t state = {0};
    ably_channel_subscribe(channel, "rewind-event", on_message, &state);

    REQUIRE(ably_channel_attach(channel) == ABLY_OK, "attach with rewind");
    REQUIRE(wait_for_chan(channel, ABLY_CHAN_ATTACHED, 10), "ATTACHED");

    /* The rewound message should arrive shortly after ATTACHED. */
    int got = wait_for_msg(&state, 1, 10);
    CHECK(got, "rewind message delivered within 10s");
    if (got) {
        CHECK(strcmp(state.last_data, "rewind-payload") == 0, "rewind payload matches");
    }

    ably_rt_client_close(rt, 5000);
    ably_rt_client_destroy(rt);
}

/* ---- Entry point ---- */

int main(void)
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key) {
        printf("SKIP: ABLY_API_KEY not set — skipping real-time integration tests.\n");
        return 0;
    }

    ably_set_log_level(ABLY_LOG_WARN);

    test_connect_and_close(api_key);
    test_channel_attach(api_key);
    test_subscribe_receive(api_key);
    test_realtime_publish(api_key);
    test_name_filter(api_key);
    test_presence_enter_and_get(api_key);
    test_presence_subscribe(api_key);
    test_rewind(api_key);

    if (g_failures == 0) {
        printf("\nAll real-time integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) failed.\n", g_failures);
    return 1;
}
