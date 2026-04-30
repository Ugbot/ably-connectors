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
 * Unit tests for presence protocol decode and the presence state machine.
 *
 * Uses canned JSON strings to drive the decoder without requiring a network
 * connection.  Verifies that PRESENCE and SYNC frames update the member map
 * and notify subscribers correctly.
 */

#include "../src/realtime/protocol.h"
#include "../src/realtime/presence.h"
#include "../src/realtime/channel.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------ */

static void check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

/* Decode a JSON frame string into a pre-allocated frame. */
static void decode_json(const char *json, ably_proto_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    ably_error_t err = ably_proto_decode_json(json, strlen(json), frame);
    check(err == ABLY_OK, "decode_json must succeed");
}

/* ---- subscriber tracking ------------------------------------------------ */

typedef struct {
    ably_presence_message_t msgs[64];
    int                     count;
} presence_log_t;

static void pres_cb(ably_channel_t *ch, const ably_presence_message_t *msg, void *ud)
{
    (void)ch;
    presence_log_t *log = ud;
    if (log->count < 64)
        log->msgs[log->count++] = *msg;
}

/* ---- fake channel stub (no realtime connection needed) ------------------ */

/* The presence module only uses ch->log and ch->pres fields. */
static struct ably_channel_s s_fake_ch;

static void fake_channel_init(void)
{
    memset(&s_fake_ch, 0, sizeof(s_fake_ch));
}

/* ---- tests -------------------------------------------------------------- */

static void test_presence_frame_decode(void)
{
    /* A PRESENCE frame with one ENTER and one LEAVE message. */
    const char *json =
        "{"
        "\"action\":14,"
        "\"channel\":\"test-ch\","
        "\"channelSerial\":\"ser001\","
        "\"presence\":["
        "  {\"action\":2,\"clientId\":\"alice\",\"connectionId\":\"conn1\",\"data\":\"hello\",\"timestamp\":1000},"
        "  {\"action\":3,\"clientId\":\"bob\",  \"connectionId\":\"conn2\",\"data\":\"\",    \"timestamp\":2000}"
        "]}";

    ably_proto_frame_t frame;
    decode_json(json, &frame);

    check(frame.action == ABLY_ACTION_PRESENCE, "action should be PRESENCE");
    check(strcmp(frame.channel, "test-ch") == 0, "channel should be test-ch");
    check(strcmp(frame.channel_serial, "ser001") == 0, "channelSerial should be ser001");
    check(frame.presence_count == 2, "presence_count should be 2");

    const ably_presence_message_t *pm0 = &frame.presence_msgs[0];
    check(pm0->action == ABLY_PRESENCE_ENTER, "first action should be ENTER");
    check(strcmp(pm0->client_id, "alice") == 0, "first clientId should be alice");
    check(strcmp(pm0->connection_id, "conn1") == 0, "first connectionId should be conn1");
    check(strcmp(pm0->data, "hello") == 0, "first data should be hello");
    check(pm0->timestamp == 1000, "first timestamp should be 1000");

    const ably_presence_message_t *pm1 = &frame.presence_msgs[1];
    check(pm1->action == ABLY_PRESENCE_LEAVE, "second action should be LEAVE");
    check(strcmp(pm1->client_id, "bob") == 0, "second clientId should be bob");

    printf("PASS: presence_frame_decode\n");
}

static void test_sync_frame_decode(void)
{
    const char *json =
        "{"
        "\"action\":16,"
        "\"channel\":\"test-ch\","
        "\"syncSerial\":\"prefix:cursor123\","
        "\"presence\":["
        "  {\"action\":1,\"clientId\":\"carol\",\"connectionId\":\"conn3\",\"data\":\"d1\",\"timestamp\":3000}"
        "]}";

    ably_proto_frame_t frame;
    decode_json(json, &frame);

    check(frame.action == ABLY_ACTION_SYNC, "action should be SYNC");
    check(strcmp(frame.sync_serial, "prefix:cursor123") == 0, "syncSerial should be set");
    check(frame.presence_count == 1, "presence_count should be 1");

    const ably_presence_message_t *pm = &frame.presence_msgs[0];
    check(pm->action == ABLY_PRESENCE_PRESENT, "action should be PRESENT");
    check(strcmp(pm->client_id, "carol") == 0, "clientId should be carol");

    printf("PASS: sync_frame_decode\n");
}

static void test_presence_state_machine(void)
{
    fake_channel_init();

    ably_presence_state_t pres;
    ably_presence_init(&pres);

    presence_log_t log;
    memset(&log, 0, sizeof(log));
    int token = ably_presence_subscribe(&pres, pres_cb, &log);
    check(token > 0, "subscribe should return positive token");

    /* ENTER alice */
    const char *enter_json =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":2,\"clientId\":\"alice\",\"data\":\"d1\",\"timestamp\":1}]}";
    ably_proto_frame_t frame;
    decode_json(enter_json, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);

    int total = 0;
    ably_presence_message_t members[8];
    int written = ably_presence_get_members(&pres, members, 8, &total);
    check(total == 1, "should have 1 present member after ENTER");
    check(written == 1, "should have written 1 member");
    check(strcmp(members[0].client_id, "alice") == 0, "member should be alice");
    check(log.count == 1, "subscriber should have been called once");
    check(log.msgs[0].action == ABLY_PRESENCE_ENTER, "subscriber should see ENTER");

    /* ENTER bob */
    const char *enter_bob =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":2,\"clientId\":\"bob\",\"data\":\"d2\",\"timestamp\":2}]}";
    decode_json(enter_bob, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);

    written = ably_presence_get_members(&pres, members, 8, &total);
    check(total == 2, "should have 2 present members");
    check(written == 2, "should have written 2 members");

    /* UPDATE alice */
    const char *update_json =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":4,\"clientId\":\"alice\",\"data\":\"new_data\",\"timestamp\":3}]}";
    decode_json(update_json, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);

    written = ably_presence_get_members(&pres, members, 8, &total);
    check(total == 2, "still 2 members after UPDATE");
    /* Find alice */
    int found_alice = 0;
    for (int i = 0; i < written; i++) {
        if (strcmp(members[i].client_id, "alice") == 0) {
            check(strcmp(members[i].data, "new_data") == 0, "alice data should be updated");
            found_alice = 1;
        }
    }
    check(found_alice, "alice should still be in members");

    /* LEAVE bob */
    const char *leave_bob =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":3,\"clientId\":\"bob\",\"data\":\"\",\"timestamp\":4}]}";
    decode_json(leave_bob, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);

    written = ably_presence_get_members(&pres, members, 8, &total);
    check(total == 1, "1 member after LEAVE");
    check(strcmp(members[0].client_id, "alice") == 0, "remaining member is alice");

    /* Unsubscribe and verify no more notifications. */
    ably_presence_unsubscribe(&pres, token);
    int prev_count = log.count;
    const char *enter_carol =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":2,\"clientId\":\"carol\",\"data\":\"\",\"timestamp\":5}]}";
    decode_json(enter_carol, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);
    check(log.count == prev_count, "no more notifications after unsubscribe");

    printf("PASS: presence_state_machine\n");
}

static void test_presence_sync(void)
{
    fake_channel_init();

    ably_presence_state_t pres;
    ably_presence_init(&pres);

    presence_log_t log;
    memset(&log, 0, sizeof(log));
    ably_presence_subscribe(&pres, pres_cb, &log);

    /* Pre-populate: alice and bob are present. */
    const char *enter_alice =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":2,\"clientId\":\"alice\",\"data\":\"a\",\"timestamp\":1}]}";
    const char *enter_bob =
        "{\"action\":14,\"channel\":\"ch\","
        "\"presence\":[{\"action\":2,\"clientId\":\"bob\",\"data\":\"b\",\"timestamp\":2}]}";
    ably_proto_frame_t frame;
    decode_json(enter_alice, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);
    decode_json(enter_bob, &frame);
    ably_presence_handle_message(&pres, &s_fake_ch, &frame);

    int total = 0;
    ably_presence_get_members(&pres, NULL, 0, &total);
    check(total == 2, "2 members before sync");

    /* SYNC page 1 of 2: cursor present → not last page.
     * Only alice appears in this SYNC page. */
    const char *sync1 =
        "{\"action\":16,\"channel\":\"ch\","
        "\"syncSerial\":\"pref:cursor1\","
        "\"presence\":[{\"action\":1,\"clientId\":\"alice\",\"data\":\"a2\",\"timestamp\":10}]}";
    decode_json(sync1, &frame);
    ably_presence_handle_sync(&pres, &s_fake_ch, &frame);

    check(pres.syncing == 1, "should still be syncing after first page");

    /* SYNC page 2 of 2: empty cursor → last page.
     * carol appears; bob does not → should be synthesized as LEAVE. */
    const char *sync2 =
        "{\"action\":16,\"channel\":\"ch\","
        "\"syncSerial\":\"pref:\","
        "\"presence\":[{\"action\":1,\"clientId\":\"carol\",\"data\":\"c\",\"timestamp\":11}]}";
    decode_json(sync2, &frame);

    int pre_sync_log = log.count;
    ably_presence_handle_sync(&pres, &s_fake_ch, &frame);

    check(pres.syncing == 0, "syncing should be 0 after last page");

    /* After sync: alice and carol should be present; bob should be gone. */
    ably_presence_message_t members[8];
    int written = ably_presence_get_members(&pres, members, 8, &total);
    check(total == 2, "2 members after sync (alice + carol)");

    int has_alice = 0, has_carol = 0, has_bob = 0;
    for (int i = 0; i < written; i++) {
        if (strcmp(members[i].client_id, "alice") == 0) has_alice = 1;
        if (strcmp(members[i].client_id, "carol") == 0) has_carol = 1;
        if (strcmp(members[i].client_id, "bob")   == 0) has_bob   = 1;
    }
    check(has_alice, "alice should be present after sync");
    check(has_carol, "carol should be present after sync");
    check(!has_bob,  "bob should have been removed by sync");

    /* Subscriber should have received a synthesized LEAVE for bob. */
    int saw_bob_leave = 0;
    for (int i = pre_sync_log; i < log.count; i++) {
        if (strcmp(log.msgs[i].client_id, "bob") == 0 &&
            log.msgs[i].action == ABLY_PRESENCE_LEAVE) {
            saw_bob_leave = 1;
        }
    }
    check(saw_bob_leave, "subscriber should have received synthesized LEAVE for bob");

    printf("PASS: presence_sync\n");
}

static void test_attached_flags_decode(void)
{
    /* ATTACHED frame with RESUMED flag (bit 2 = 4) and HAS_PRESENCE (bit 0 = 1). */
    const char *json =
        "{\"action\":11,\"channel\":\"my-ch\",\"flags\":5,\"channelSerial\":\"ser999\"}";

    ably_proto_frame_t frame;
    decode_json(json, &frame);

    check(frame.action == ABLY_ACTION_ATTACHED, "action should be ATTACHED");
    check(frame.flags & ABLY_FLAG_RESUMED, "RESUMED flag should be set");
    check(frame.flags & ABLY_FLAG_HAS_PRESENCE, "HAS_PRESENCE flag should be set");
    check(strcmp(frame.channel_serial, "ser999") == 0, "channelSerial should be set");

    printf("PASS: attached_flags_decode\n");
}

int main(void)
{
    test_presence_frame_decode();
    test_sync_frame_decode();
    test_presence_state_machine();
    test_presence_sync();
    test_attached_flags_decode();

    printf("All presence protocol tests passed.\n");
    return 0;
}
