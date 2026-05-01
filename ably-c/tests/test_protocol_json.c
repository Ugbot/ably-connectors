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

#include "../src/realtime/protocol.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_encode_heartbeat(void)
{
    char buf[64];
    size_t n = ably_proto_encode_heartbeat_json(buf, sizeof(buf));
    CHECK(n > 0, "heartbeat encode non-zero");
    CHECK(strstr(buf, "\"action\":0") != NULL, "heartbeat has action=0");
}

static void test_encode_attach(void)
{
    char buf[128];
    size_t n = ably_proto_encode_attach_json(buf, sizeof(buf), "test-channel", 0);
    CHECK(n > 0, "attach encode non-zero");
    CHECK(strstr(buf, "\"action\":10") != NULL, "attach has action=10");
    CHECK(strstr(buf, "test-channel") != NULL, "attach has channel name");
}

static void test_encode_close(void)
{
    char buf[64];
    size_t n = ably_proto_encode_close_json(buf, sizeof(buf));
    CHECK(n > 0, "close encode non-zero");
    CHECK(strstr(buf, "\"action\":7") != NULL, "close has action=7");
}

static void test_encode_publish(void)
{
    char buf[256];
    size_t n = ably_proto_encode_publish_json(buf, sizeof(buf),
                                               "events", "greet", "hello", 0);
    CHECK(n > 0, "publish encode non-zero");
    CHECK(strstr(buf, "\"action\":15") != NULL, "publish has action=15");
    CHECK(strstr(buf, "events") != NULL, "publish has channel");
    CHECK(strstr(buf, "greet") != NULL, "publish has name");
    CHECK(strstr(buf, "hello") != NULL, "publish has data");
}

static void test_encode_truncation(void)
{
    char buf[4];
    size_t n = ably_proto_encode_heartbeat_json(buf, sizeof(buf));
    CHECK(n == 0, "truncated encode returns 0");
}

static void test_decode_connected(void)
{
    const char *json = "{\"action\":4,\"connectionId\":\"abc123\"}";
    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode CONNECTED returns OK");
    CHECK(frame.action == ABLY_ACTION_CONNECTED, "decoded action=CONNECTED");
}

static void test_decode_message(void)
{
    const char *json =
        "{\"action\":15,\"channel\":\"test\","
        "\"messages\":[{\"id\":\"m1\",\"name\":\"ev\",\"data\":\"hi\",\"timestamp\":1700000000000}]}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode MESSAGE returns OK");
    CHECK(frame.action == ABLY_ACTION_MESSAGE, "decoded action=MESSAGE");
    CHECK(frame.channel && strcmp(frame.channel, "test") == 0, "decoded channel");
    CHECK(frame.message_count == 1, "decoded 1 message");
    CHECK(msgs[0].name && strcmp(msgs[0].name, "ev") == 0, "decoded message name");
    CHECK(msgs[0].data && strcmp(msgs[0].data, "hi") == 0, "decoded message data");
    CHECK(msgs[0].timestamp == 1700000000000LL, "decoded timestamp");
}

static void test_decode_error(void)
{
    const char *json =
        "{\"action\":9,\"error\":{\"code\":40100,\"message\":\"Unauthorized\"}}";
    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode ERROR frame returns OK");
    CHECK(frame.action == ABLY_ACTION_ERROR, "decoded action=ERROR");
    CHECK(frame.error_code == 40100, "decoded error code 40100");
    CHECK(frame.error_message && strcmp(frame.error_message, "Unauthorized") == 0,
          "decoded error message");
}

static void test_decode_invalid(void)
{
    const char *json = "not valid json!!!";
    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_ERR_PROTOCOL, "invalid JSON returns ABLY_ERR_PROTOCOL");
}

static void test_decode_multiple_messages(void)
{
    const char *json =
        "{\"action\":15,\"channel\":\"ch\","
        "\"messages\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"}]}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode 3-message frame OK");
    CHECK(frame.message_count == 3, "decoded 3 messages");
    CHECK(msgs[0].name && strcmp(msgs[0].name, "a") == 0, "message[0].name=a");
    CHECK(msgs[1].name && strcmp(msgs[1].name, "b") == 0, "message[1].name=b");
    CHECK(msgs[2].name && strcmp(msgs[2].name, "c") == 0, "message[2].name=c");
}

static void test_decode_cap_respected(void)
{
    const char *json =
        "{\"action\":15,\"channel\":\"ch\","
        "\"messages\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"}]}";

    ably_proto_message_t msgs[2];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 2;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode with cap=2 OK");
    CHECK(frame.message_count == 2, "only 2 messages decoded when cap=2");
}

static void test_roundtrip_heartbeat(void)
{
    char buf[64];
    size_t n = ably_proto_encode_heartbeat_json(buf, sizeof(buf));
    CHECK(n > 0, "heartbeat encode succeeds");

    ably_proto_frame_t frame = {0};
    ably_error_t err = ably_proto_decode_json(buf, n, &frame);
    CHECK(err == ABLY_OK, "heartbeat decode succeeds");
    CHECK(frame.action == ABLY_ACTION_HEARTBEAT, "roundtrip heartbeat action=0");
}

/* New tests for presence, flags, channelSerial, syncSerial, occupancy, attach params. */

static void test_decode_attached_flags(void)
{
    /* flags=5: HAS_PRESENCE(1) | RESUMED(4) */
    const char *json =
        "{\"action\":11,\"channel\":\"flags-test\","
        "\"channelSerial\":\"serial-abc\",\"flags\":5}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode ATTACHED with flags returns OK");
    CHECK(frame.action == ABLY_ACTION_ATTACHED, "ATTACHED action=11");
    CHECK(frame.flags == 5, "flags decoded as 5");
    CHECK((frame.flags & ABLY_FLAG_HAS_PRESENCE) != 0, "HAS_PRESENCE flag set");
    CHECK((frame.flags & ABLY_FLAG_RESUMED)      != 0, "RESUMED flag set");
    CHECK((frame.flags & ABLY_FLAG_HAS_BACKLOG)  == 0, "HAS_BACKLOG flag not set");
    CHECK(strcmp(frame.channel_serial, "serial-abc") == 0, "channelSerial decoded");
}

static void test_decode_message_channel_serial(void)
{
    const char *json =
        "{\"action\":15,\"channel\":\"ch\",\"channelSerial\":\"msg-serial-xyz\","
        "\"messages\":[{\"name\":\"ev\",\"data\":\"d\",\"timestamp\":1700000000000}]}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode MESSAGE with channelSerial OK");
    CHECK(strcmp(frame.channel_serial, "msg-serial-xyz") == 0,
          "channelSerial decoded from MESSAGE frame");
}

static void test_decode_presence_array(void)
{
    const char *json =
        "{\"action\":14,\"channel\":\"pres-ch\","
        "\"presence\":["
        "  {\"action\":2,\"clientId\":\"alice\",\"data\":\"hello\",\"timestamp\":1700000000000},"
        "  {\"action\":3,\"clientId\":\"bob\",  \"data\":\"\",     \"timestamp\":1700000001000}"
        "]}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode PRESENCE frame returns OK");
    CHECK(frame.action == ABLY_ACTION_PRESENCE, "action=PRESENCE");
    CHECK(frame.presence_count == 2, "2 presence messages decoded");
    CHECK(frame.presence_msgs[0].action == ABLY_PRESENCE_ENTER,   "presence[0] action=ENTER");
    CHECK(strcmp(frame.presence_msgs[0].client_id, "alice") == 0,  "presence[0] clientId=alice");
    CHECK(strcmp(frame.presence_msgs[0].data, "hello") == 0,       "presence[0] data=hello");
    CHECK(frame.presence_msgs[0].timestamp == 1700000000000LL,     "presence[0] timestamp");
    CHECK(frame.presence_msgs[1].action == ABLY_PRESENCE_LEAVE,   "presence[1] action=LEAVE");
    CHECK(strcmp(frame.presence_msgs[1].client_id, "bob") == 0,   "presence[1] clientId=bob");
}

static void test_decode_sync_serial(void)
{
    const char *json =
        "{\"action\":16,\"channel\":\"sync-ch\","
        "\"syncSerial\":\"prefix:cursor123\","
        "\"presence\":["
        "  {\"action\":1,\"clientId\":\"charlie\",\"data\":\"d\",\"timestamp\":1700000000000}"
        "]}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode SYNC frame returns OK");
    CHECK(frame.action == ABLY_ACTION_SYNC, "action=SYNC");
    CHECK(strcmp(frame.sync_serial, "prefix:cursor123") == 0, "syncSerial decoded");
    CHECK(frame.presence_count == 1, "1 presence message in SYNC frame");
    CHECK(frame.presence_msgs[0].action == ABLY_PRESENCE_PRESENT, "presence[0] action=PRESENT");
    CHECK(strcmp(frame.presence_msgs[0].client_id, "charlie") == 0, "presence[0] clientId=charlie");
}

static void test_decode_occupancy_extras(void)
{
    const char *json =
        "{\"action\":15,\"channel\":\"occ-ch\","
        "\"messages\":[{"
        "  \"name\":\"[meta]occupancy\","
        "  \"extras\":{\"occupancy\":{\"metrics\":{"
        "    \"connections\":10,\"publishers\":3,\"subscribers\":7,"
        "    \"presenceConnections\":2,\"presenceMembers\":5,\"presenceSubscribers\":4"
        "  }}},"
        "  \"timestamp\":1700000000000"
        "}]}";

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_json(json, strlen(json), &frame);
    CHECK(err == ABLY_OK, "decode MESSAGE with occupancy extras OK");
    CHECK(frame.message_count == 1, "1 message decoded");
    CHECK(msgs[0].has_occupancy == 1, "has_occupancy flag set");
    CHECK(msgs[0].occupancy.connections          == 10, "connections=10");
    CHECK(msgs[0].occupancy.publishers           ==  3, "publishers=3");
    CHECK(msgs[0].occupancy.subscribers          ==  7, "subscribers=7");
    CHECK(msgs[0].occupancy.presence_connections ==  2, "presenceConnections=2");
    CHECK(msgs[0].occupancy.presence_members     ==  5, "presenceMembers=5");
    CHECK(msgs[0].occupancy.presence_subscribers ==  4, "presenceSubscribers=4");
}

static void test_encode_attach_params_rewind(void)
{
    ably_attach_params_t params = {0};
    params.rewind = 5;

    char buf[512];
    size_t n = ably_proto_encode_attach_json(buf, sizeof(buf), "rewind-ch", &params);
    CHECK(n > 0, "attach with rewind encode non-zero");
    CHECK(strstr(buf, "\"action\":10")    != NULL, "action=10");
    CHECK(strstr(buf, "rewind-ch")        != NULL, "channel name present");
    CHECK(strstr(buf, "rewind")           != NULL, "rewind param present");
    CHECK(strstr(buf, "\"5\"")            != NULL, "rewind value=\"5\"");
}

static void test_encode_attach_params_occupancy(void)
{
    ably_attach_params_t params = {0};
    params.occupancy = 1;

    char buf[512];
    size_t n = ably_proto_encode_attach_json(buf, sizeof(buf), "occ-ch", &params);
    CHECK(n > 0, "attach with occupancy encode non-zero");
    CHECK(strstr(buf, "occupancy")        != NULL, "occupancy param present");
    CHECK(strstr(buf, "metrics.all")      != NULL, "occupancy value=metrics.all");
}

static void test_encode_attach_params_channel_serial(void)
{
    ably_attach_params_t params = {0};
    params.channel_serial = "resume-serial-xyz";

    char buf[512];
    size_t n = ably_proto_encode_attach_json(buf, sizeof(buf), "gap-ch", &params);
    CHECK(n > 0, "attach with channelSerial encode non-zero");
    CHECK(strstr(buf, "channelSerial")    != NULL, "channelSerial key present");
    CHECK(strstr(buf, "resume-serial-xyz") != NULL, "channelSerial value present");
}

static void test_encode_attach_params_combined(void)
{
    ably_attach_params_t params = {0};
    params.delta    = 1;
    params.rewind   = 10;
    params.occupancy = 1;
    params.channel_serial = "combo-serial";

    char buf[1024];
    size_t n = ably_proto_encode_attach_json(buf, sizeof(buf), "combo-ch", &params);
    CHECK(n > 0, "attach with all params encode non-zero");
    CHECK(strstr(buf, "vcdiff")       != NULL, "delta=vcdiff present");
    CHECK(strstr(buf, "rewind")       != NULL, "rewind present");
    CHECK(strstr(buf, "metrics.all")  != NULL, "occupancy present");
    CHECK(strstr(buf, "combo-serial") != NULL, "channelSerial present");
}

int main(void)
{
    test_encode_heartbeat();
    test_encode_attach();
    test_encode_close();
    test_encode_publish();
    test_encode_truncation();

    test_decode_connected();
    test_decode_message();
    test_decode_error();
    test_decode_invalid();
    test_decode_multiple_messages();
    test_decode_cap_respected();

    test_roundtrip_heartbeat();

    /* New tests */
    test_decode_attached_flags();
    test_decode_message_channel_serial();
    test_decode_presence_array();
    test_decode_sync_serial();
    test_decode_occupancy_extras();
    test_encode_attach_params_rewind();
    test_encode_attach_params_occupancy();
    test_encode_attach_params_channel_serial();
    test_encode_attach_params_combined();

    if (failures == 0) {
        printf("All JSON protocol tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
