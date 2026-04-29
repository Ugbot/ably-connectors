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
#include <stdint.h>
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
    uint8_t buf[64];
    size_t n = ably_proto_encode_heartbeat_msgpack(buf, sizeof(buf));
    CHECK(n > 0, "msgpack heartbeat encode non-zero");
}

static void test_encode_attach(void)
{
    uint8_t buf[128];
    size_t n = ably_proto_encode_attach_msgpack(buf, sizeof(buf), "my-channel");
    CHECK(n > 0, "msgpack attach encode non-zero");
}

static void test_encode_publish(void)
{
    uint8_t buf[256];
    size_t n = ably_proto_encode_publish_msgpack(buf, sizeof(buf),
                                                   "chan", "evt", "payload");
    CHECK(n > 0, "msgpack publish encode non-zero");
}

static void test_roundtrip_heartbeat(void)
{
    uint8_t buf[64];
    size_t n = ably_proto_encode_heartbeat_msgpack(buf, sizeof(buf));
    CHECK(n > 0, "msgpack heartbeat encode");

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_msgpack(buf, n, &frame);
    CHECK(err == ABLY_OK, "msgpack heartbeat decode OK");
    CHECK(frame.action == ABLY_ACTION_HEARTBEAT, "roundtrip action=HEARTBEAT");
}

static void test_roundtrip_attach(void)
{
    uint8_t buf[128];
    size_t n = ably_proto_encode_attach_msgpack(buf, sizeof(buf), "events");
    CHECK(n > 0, "msgpack attach encode");

    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_msgpack(buf, n, &frame);
    CHECK(err == ABLY_OK, "msgpack attach roundtrip decode OK");
    CHECK(frame.action == ABLY_ACTION_ATTACH, "roundtrip action=ATTACH");
    CHECK(frame.channel && strcmp(frame.channel, "events") == 0,
          "roundtrip channel name");
}

static void test_decode_invalid(void)
{
    uint8_t buf[] = {0xFF, 0xFF, 0xFF};  /* invalid msgpack */
    ably_proto_message_t msgs[8];
    ably_proto_frame_t frame = {0};
    frame.messages    = msgs;
    frame.message_cap = 8;

    ably_error_t err = ably_proto_decode_msgpack(buf, sizeof(buf), &frame);
    CHECK(err == ABLY_ERR_PROTOCOL, "invalid msgpack returns ABLY_ERR_PROTOCOL");
}

int main(void)
{
    test_encode_heartbeat();
    test_encode_attach();
    test_encode_publish();

    test_roundtrip_heartbeat();
    test_roundtrip_attach();
    test_decode_invalid();

    if (failures == 0) {
        printf("All MessagePack protocol tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
