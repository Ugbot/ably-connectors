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

    if (g_failures == 0) {
        printf("\nAll real-time integration tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) failed.\n", g_failures);
    return 1;
}
