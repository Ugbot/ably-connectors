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

    if (failures == 0) {
        printf("All JSON protocol tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
