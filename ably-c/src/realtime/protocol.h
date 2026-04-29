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

#ifndef ABLY_PROTOCOL_H
#define ABLY_PROTOCOL_H

#include "ably/ably_types.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Ably real-time protocol action codes.
 */
typedef enum {
    ABLY_ACTION_HEARTBEAT    = 0,
    ABLY_ACTION_ACK          = 1,
    ABLY_ACTION_NACK         = 2,
    ABLY_ACTION_CONNECTED    = 4,
    ABLY_ACTION_DISCONNECT   = 5,
    ABLY_ACTION_DISCONNECTED = 6,
    ABLY_ACTION_CLOSE        = 7,
    ABLY_ACTION_CLOSED       = 8,
    ABLY_ACTION_ERROR        = 9,
    ABLY_ACTION_ATTACH       = 10,
    ABLY_ACTION_ATTACHED     = 11,
    ABLY_ACTION_DETACH       = 12,
    ABLY_ACTION_DETACHED     = 13,
    ABLY_ACTION_PRESENCE     = 14,
    ABLY_ACTION_MESSAGE      = 15,
    ABLY_ACTION_SYNC         = 16,
    ABLY_ACTION_AUTH         = 17,
} ably_action_t;

/*
 * A single decoded Ably message.
 * Pointer fields reference the inbound frame buffer — valid only while that
 * buffer is live.  Copy if retention beyond the callback is needed.
 */
typedef struct {
    const char *id;
    const char *client_id;
    const char *connection_id;
    const char *name;
    const char *data;
    const char *encoding;
    int64_t     timestamp;

    /* Delta compression fields (from message.extras.delta.*).
     * Both are NULL for non-delta messages. */
    const char *delta_format;   /* e.g. "vcdiff" */
    const char *delta_from;     /* ID of the base message */
} ably_proto_message_t;

/*
 * String pool size for the msgpack decoder.
 * All strings decoded from a MessagePack frame are copied (NUL-terminated)
 * into this pool.  JSON decode points directly into cJSON's storage instead.
 */
#define ABLY_PROTO_STRING_POOL_SIZE 4096

/*
 * A decoded inbound protocol frame.
 *
 * messages[] is a caller-supplied fixed-capacity array.  On decode, up to
 * message_cap entries are written and message_count is set.  No allocation.
 *
 * string_pool is used by the msgpack decoder to NUL-terminate strings.
 * JSON decode points directly into cJSON's internal buffers instead.
 */
typedef struct {
    ably_action_t       action;
    const char         *channel;
    int64_t             msg_serial;
    int                 count;
    int                 error_code;
    const char         *error_message;
    int                 flags;

    ably_proto_message_t *messages;    /* caller-provided array */
    size_t                message_count;
    size_t                message_cap;

    /* String pool: msgpack decode writes NUL-terminated copies here. */
    char   string_pool[ABLY_PROTO_STRING_POOL_SIZE];
    size_t string_pool_used;
} ably_proto_frame_t;

/* ---------------------------------------------------------------------------
 * Encode — write into caller-supplied buffers; no allocation.
 * Returns bytes written, or 0 on truncation.
 * --------------------------------------------------------------------------- */

size_t ably_proto_encode_heartbeat_json   (char    *buf, size_t len);
size_t ably_proto_encode_heartbeat_msgpack(uint8_t *buf, size_t len);

/* delta != 0 adds params:{"delta":"vcdiff"} to the ATTACH frame. */
size_t ably_proto_encode_attach_json    (char    *buf, size_t len, const char *channel, int delta);
size_t ably_proto_encode_attach_msgpack (uint8_t *buf, size_t len, const char *channel, int delta);

size_t ably_proto_encode_detach_json    (char    *buf, size_t len, const char *channel);
size_t ably_proto_encode_detach_msgpack (uint8_t *buf, size_t len, const char *channel);

size_t ably_proto_encode_close_json    (char    *buf, size_t len);
size_t ably_proto_encode_close_msgpack (uint8_t *buf, size_t len);

size_t ably_proto_encode_publish_json   (char    *buf, size_t len,
                                          const char *channel,
                                          const char *name,
                                          const char *data,
                                          int64_t     msg_serial);
size_t ably_proto_encode_publish_msgpack(uint8_t *buf, size_t len,
                                          const char *channel,
                                          const char *name,
                                          const char *data,
                                          int64_t     msg_serial);

/* ---------------------------------------------------------------------------
 * Encoding-aware helpers — select JSON or MessagePack at runtime.
 * buf must be large enough for either format (use ABLY_MAX_FRAME_SIZE).
 * --------------------------------------------------------------------------- */
#include "ably/ably_types.h"
#include <stdint.h>

static inline size_t ably_proto_encode_heartbeat(char *buf, size_t len,
                                                  ably_encoding_t enc)
{
    if (enc == ABLY_ENCODING_MSGPACK)
        return ably_proto_encode_heartbeat_msgpack((uint8_t *)buf, len);
    return ably_proto_encode_heartbeat_json(buf, len);
}

static inline size_t ably_proto_encode_attach(char *buf, size_t len,
                                               const char *channel,
                                               int delta,
                                               ably_encoding_t enc)
{
    if (enc == ABLY_ENCODING_MSGPACK)
        return ably_proto_encode_attach_msgpack((uint8_t *)buf, len, channel, delta);
    return ably_proto_encode_attach_json(buf, len, channel, delta);
}

static inline size_t ably_proto_encode_detach(char *buf, size_t len,
                                               const char *channel,
                                               ably_encoding_t enc)
{
    if (enc == ABLY_ENCODING_MSGPACK)
        return ably_proto_encode_detach_msgpack((uint8_t *)buf, len, channel);
    return ably_proto_encode_detach_json(buf, len, channel);
}

static inline size_t ably_proto_encode_close(char *buf, size_t len,
                                              ably_encoding_t enc)
{
    if (enc == ABLY_ENCODING_MSGPACK)
        return ably_proto_encode_close_msgpack((uint8_t *)buf, len);
    return ably_proto_encode_close_json(buf, len);
}

static inline size_t ably_proto_encode_publish(char *buf, size_t len,
                                                const char *channel,
                                                const char *name,
                                                const char *data,
                                                int64_t     msg_serial,
                                                ably_encoding_t enc)
{
    if (enc == ABLY_ENCODING_MSGPACK)
        return ably_proto_encode_publish_msgpack((uint8_t *)buf, len,
                                                  channel, name, data, msg_serial);
    return ably_proto_encode_publish_json(buf, len, channel, name, data, msg_serial);
}

/* ---------------------------------------------------------------------------
 * Decode — parse inbound frames; no allocation.
 * String pointers in frame output reference buf — do not free buf while in use.
 * Returns ABLY_OK or ABLY_ERR_PROTOCOL.
 * --------------------------------------------------------------------------- */

ably_error_t ably_proto_decode_json   (const char    *buf, size_t len,
                                        ably_proto_frame_t *frame);
ably_error_t ably_proto_decode_msgpack(const uint8_t *buf, size_t len,
                                        ably_proto_frame_t *frame);

#endif /* ABLY_PROTOCOL_H */
