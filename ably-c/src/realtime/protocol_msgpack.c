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
 * MessagePack encode/decode for the Ably real-time protocol.
 *
 * The Ably MessagePack wire format uses fixmap keys that mirror the JSON field
 * names ("action", "channel", "messages", etc.).
 *
 * Encode: mpack writer writes directly into a caller-supplied buffer (no alloc).
 * Decode: mpack tree reader parses the binary; output pointers reference mpack's
 *         internal node storage.  Like the JSON decoder, only one frame at a time
 *         is live because this is a single-threaded service loop.
 */

#include "protocol.h"
#include "mpack.h"

#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------------------------
 * Encode helpers (mpack writer into fixed buffer)
 * --------------------------------------------------------------------------- */

static size_t encode_simple(uint8_t *buf, size_t len, int action)
{
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf, len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "action");
    mpack_write_int(&w, action);
    mpack_complete_map(&w);
    mpack_error_t err = mpack_writer_destroy(&w);
    if (err != mpack_ok) return 0;
    return mpack_writer_buffer_used(&w);
}

static size_t encode_with_channel(uint8_t *buf, size_t len,
                                   int action, const char *channel)
{
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf, len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "action");
    mpack_write_int(&w, action);
    mpack_write_cstr(&w, "channel");
    mpack_write_cstr(&w, channel);
    mpack_complete_map(&w);
    mpack_error_t err = mpack_writer_destroy(&w);
    if (err != mpack_ok) return 0;
    return mpack_writer_buffer_used(&w);
}

size_t ably_proto_encode_heartbeat_msgpack(uint8_t *buf, size_t len)
{
    return encode_simple(buf, len, ABLY_ACTION_HEARTBEAT);
}

size_t ably_proto_encode_attach_msgpack(uint8_t *buf, size_t len,
                                         const char *channel, int delta)
{
    if (!delta)
        return encode_with_channel(buf, len, ABLY_ACTION_ATTACH, channel);

    /* Include params:{"delta":"vcdiff"} */
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf, len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "action");
    mpack_write_int(&w, ABLY_ACTION_ATTACH);
    mpack_write_cstr(&w, "channel");
    mpack_write_cstr(&w, channel ? channel : "");
    mpack_write_cstr(&w, "params");
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "delta");
    mpack_write_cstr(&w, "vcdiff");
    mpack_finish_map(&w);
    mpack_complete_map(&w);
    mpack_error_t err = mpack_writer_destroy(&w);
    if (err != mpack_ok) return 0;
    return mpack_writer_buffer_used(&w);
}

size_t ably_proto_encode_detach_msgpack(uint8_t *buf, size_t len,
                                         const char *channel)
{
    return encode_with_channel(buf, len, ABLY_ACTION_DETACH, channel);
}

size_t ably_proto_encode_close_msgpack(uint8_t *buf, size_t len)
{
    return encode_simple(buf, len, ABLY_ACTION_CLOSE);
}

size_t ably_proto_encode_publish_msgpack(uint8_t *buf, size_t len,
                                          const char *channel,
                                          const char *name,
                                          const char *data,
                                          int64_t     msg_serial)
{
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf, len);

    mpack_build_map(&w);
    mpack_write_cstr(&w, "action");
    mpack_write_int(&w, ABLY_ACTION_MESSAGE);
    mpack_write_cstr(&w, "msgSerial");
    mpack_write_i64(&w, msg_serial);

    if (channel) {
        mpack_write_cstr(&w, "channel");
        mpack_write_cstr(&w, channel);
    }

    mpack_write_cstr(&w, "messages");
    mpack_start_array(&w, 1);
    mpack_build_map(&w);
    if (name) {
        mpack_write_cstr(&w, "name");
        mpack_write_cstr(&w, name);
    }
    if (data) {
        mpack_write_cstr(&w, "data");
        mpack_write_cstr(&w, data);
    }
    mpack_complete_map(&w);
    mpack_finish_array(&w);
    mpack_complete_map(&w);

    mpack_error_t err = mpack_writer_destroy(&w);
    if (err != mpack_ok) return 0;
    return mpack_writer_buffer_used(&w);
}

/* ---------------------------------------------------------------------------
 * Decode
 * --------------------------------------------------------------------------- */

/* mpack tree node for the current frame — freed on next decode call. */
static mpack_tree_t s_tree;
static int          s_tree_init = 0;

static void free_last_tree(void)
{
    if (s_tree_init) {
        mpack_tree_destroy(&s_tree);
        s_tree_init = 0;
    }
}

static int64_t node_int64(mpack_node_t node, const char *key, int64_t def)
{
    mpack_node_t child = mpack_node_map_cstr_optional(node, key);
    if (mpack_node_type(child) == mpack_type_nil) return def;
    return mpack_node_i64(child);
}

/*
 * Copy a mpack string node into frame->string_pool (NUL-terminated).
 * Returns a pointer into the pool, or NULL on overflow or wrong type.
 * mpack strings are NOT NUL-terminated in the wire buffer; we must copy.
 */
static const char *pool_str(mpack_node_t node, const char *key,
                              ably_proto_frame_t *frame)
{
    mpack_node_t child = mpack_node_map_cstr_optional(node, key);
    if (mpack_node_type(child) != mpack_type_str) return NULL;

    size_t slen = mpack_node_strlen(child);
    size_t remaining = ABLY_PROTO_STRING_POOL_SIZE - frame->string_pool_used;
    if (slen + 1 > remaining) return NULL;  /* pool full — caller gets NULL */

    char *dest = frame->string_pool + frame->string_pool_used;
    memcpy(dest, mpack_node_str(child), slen);
    dest[slen] = '\0';
    frame->string_pool_used += slen + 1;
    return dest;
}

ably_error_t ably_proto_decode_msgpack(const uint8_t *buf, size_t len,
                                        ably_proto_frame_t *frame)
{
    assert(buf != NULL);
    assert(frame != NULL);

    free_last_tree();

    /* Reset string pool for this decode. */
    frame->string_pool_used = 0;

    mpack_tree_init_data(&s_tree, (const char *)buf, len);
    mpack_tree_parse(&s_tree);
    if (mpack_tree_error(&s_tree) != mpack_ok) {
        free_last_tree();
        return ABLY_ERR_PROTOCOL;
    }
    s_tree_init = 1;

    mpack_node_t root = mpack_tree_root(&s_tree);
    if (mpack_node_type(root) != mpack_type_map) return ABLY_ERR_PROTOCOL;

    frame->action         = (ably_action_t)(int)node_int64(root, "action", -1);
    frame->channel        = pool_str(root, "channel", frame);
    frame->msg_serial     = node_int64(root, "msgSerial", 0);
    frame->count          = (int)node_int64(root, "count", 0);
    frame->flags          = (int)node_int64(root, "flags", 0);
    frame->error_code     = 0;
    frame->error_message  = NULL;
    frame->message_count  = 0;
    frame->connection_id  = pool_str(root, "connectionId",  frame);
    frame->connection_key = pool_str(root, "connectionKey", frame);

    if ((int)frame->action < 0) return ABLY_ERR_PROTOCOL;

    /* Error object */
    mpack_node_t err_node = mpack_node_map_cstr_optional(root, "error");
    if (mpack_node_type(err_node) == mpack_type_map) {
        frame->error_code    = (int)node_int64(err_node, "code", 0);
        frame->error_message = pool_str(err_node, "message", frame);
    }

    /* Messages array */
    mpack_node_t msgs_node = mpack_node_map_cstr_optional(root, "messages");
    if (mpack_node_type(msgs_node) == mpack_type_array
        && frame->messages && frame->message_cap > 0) {

        size_t nmsg = mpack_node_array_length(msgs_node);
        for (size_t i = 0; i < nmsg && frame->message_count < frame->message_cap; i++) {
            mpack_node_t m = mpack_node_array_at(msgs_node, i);
            ably_proto_message_t *pm = &frame->messages[frame->message_count++];
            pm->id            = pool_str(m, "id",           frame);
            pm->client_id     = pool_str(m, "clientId",     frame);
            pm->connection_id = pool_str(m, "connectionId", frame);
            pm->name          = pool_str(m, "name",         frame);
            pm->data          = pool_str(m, "data",         frame);
            pm->encoding      = pool_str(m, "encoding",     frame);
            pm->timestamp     = node_int64(m, "timestamp", 0);
            pm->delta_format  = NULL;
            pm->delta_from    = NULL;

            /* extras.delta.{format,from} */
            mpack_node_t extras = mpack_node_map_cstr_optional(m, "extras");
            if (mpack_node_type(extras) == mpack_type_map) {
                mpack_node_t delta_node = mpack_node_map_cstr_optional(extras, "delta");
                if (mpack_node_type(delta_node) == mpack_type_map) {
                    pm->delta_format = pool_str(delta_node, "format", frame);
                    pm->delta_from   = pool_str(delta_node, "from",   frame);
                }
            }
        }
    }

    return ABLY_OK;
}
