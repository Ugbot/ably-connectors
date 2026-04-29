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
 * JSON encode/decode for the Ably real-time protocol.
 *
 * Encode functions write into caller-supplied fixed-size buffers (no alloc).
 * Decode uses cJSON but frames the output to point into cJSON's internal
 * string storage — callers must treat decoded strings as read-only and
 * not retain them after the next decode call (they share the same scratch buf).
 *
 * All encode functions use snprintf-family calls to guarantee NUL termination
 * and truncation safety.
 */

#include "protocol.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------------------------
 * Encode helpers
 * --------------------------------------------------------------------------- */

/*
 * Write {"action":<n>} into buf.
 * Returns bytes written (excluding NUL), 0 on truncation.
 */
static size_t encode_action_only(char *buf, size_t len, int action)
{
    int n = snprintf(buf, len, "{\"action\":%d}", action);
    if (n < 0 || (size_t)n >= len) return 0;
    return (size_t)n;
}

/*
 * Write {"action":<n>,"channel":"<ch>"} into buf.
 * The channel name is JSON-escaped by replacing \ and " with their escapes.
 * Ably channel names are restricted to printable ASCII so full JSON escaping
 * is not required, but we handle the two characters that would break JSON.
 */
static size_t encode_action_channel(char *buf, size_t len,
                                     int action, const char *channel)
{
    assert(channel != NULL);
    int n = snprintf(buf, len, "{\"action\":%d,\"channel\":\"", action);
    if (n < 0 || (size_t)n >= len) return 0;
    size_t pos = (size_t)n;

    for (const char *p = channel; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (pos + 2 >= len) return 0;
            buf[pos++] = '\\';
        }
        if (pos + 1 >= len) return 0;
        buf[pos++] = *p;
    }

    if (pos + 2 >= len) return 0;
    buf[pos++] = '"';
    buf[pos++] = '}';
    buf[pos]   = '\0';
    return pos;
}

size_t ably_proto_encode_heartbeat_json(char *buf, size_t len)
{
    return encode_action_only(buf, len, ABLY_ACTION_HEARTBEAT);
}

size_t ably_proto_encode_attach_json(char *buf, size_t len, const char *channel)
{
    return encode_action_channel(buf, len, ABLY_ACTION_ATTACH, channel);
}

size_t ably_proto_encode_detach_json(char *buf, size_t len, const char *channel)
{
    return encode_action_channel(buf, len, ABLY_ACTION_DETACH, channel);
}

size_t ably_proto_encode_close_json(char *buf, size_t len)
{
    return encode_action_only(buf, len, ABLY_ACTION_CLOSE);
}

size_t ably_proto_encode_publish_json(char *buf, size_t len,
                                       const char *channel,
                                       const char *name,
                                       const char *data,
                                       int64_t     msg_serial)
{
    /*
     * Outbound publish frame:
     * {"action":15,"msgSerial":<n>,"channel":"<ch>","messages":[{"name":"<n>","data":"<d>"}]}
     *
     * msgSerial is required by the Ably protocol for all client-originated messages
     * that require ACK.  It increments monotonically within the connection lifetime.
     *
     * We use cJSON here because name/data may contain arbitrary Unicode content
     * that needs proper JSON string escaping.
     */
    cJSON *root = cJSON_CreateObject();
    if (!root) return 0;

    cJSON_AddNumberToObject(root, "action",    ABLY_ACTION_MESSAGE);
    cJSON_AddNumberToObject(root, "msgSerial", (double)msg_serial);
    if (channel) cJSON_AddStringToObject(root, "channel", channel);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg  = cJSON_CreateObject();
    if (name) cJSON_AddStringToObject(msg, "name", name);
    if (data) cJSON_AddStringToObject(msg, "data", data);
    cJSON_AddItemToArray(msgs, msg);

    char *serialised = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialised) return 0;

    size_t slen = strlen(serialised);
    if (slen >= len) {
        /* cJSON uses its own allocator; free via cJSON_free */
        cJSON_free(serialised);
        return 0;
    }
    memcpy(buf, serialised, slen + 1);
    cJSON_free(serialised);
    return slen;
}

/* ---------------------------------------------------------------------------
 * Decode
 *
 * We parse the JSON with cJSON, then populate frame fields by pointing at
 * cJSON's internal string objects.  The cJSON root is stored in a static
 * scratch slot so that consecutive decode calls reuse the allocator without
 * caller coordination.  This means only ONE decoded frame is valid at a time
 * per thread — which is exactly the service-thread access pattern.
 *
 * The decode functions are only ever called from the single service thread,
 * so the static is safe.
 * --------------------------------------------------------------------------- */

/* Thread-local would be cleaner but adds complexity; service-thread-only pattern
 * makes a simple static correct. */
static cJSON *s_last_root = NULL;

static void free_last_root(void)
{
    if (s_last_root) {
        cJSON_Delete(s_last_root);
        s_last_root = NULL;
    }
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

static int64_t json_int64(cJSON *obj, const char *key, int64_t def)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) return (int64_t)item->valuedouble;
    return def;
}

ably_error_t ably_proto_decode_json(const char *buf, size_t len,
                                     ably_proto_frame_t *frame)
{
    assert(buf != NULL);
    assert(frame != NULL);

    free_last_root();

    /* cJSON expects a NUL-terminated string; buf from wslay is always NUL-
     * terminated for text frames, but we honour len as well. */
    (void)len; /* cJSON stops at NUL */

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ABLY_ERR_PROTOCOL;
    s_last_root = root;

    cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!action_item || !cJSON_IsNumber(action_item)) return ABLY_ERR_PROTOCOL;

    frame->action      = (ably_action_t)(int)action_item->valuedouble;
    frame->channel     = json_str(root, "channel");
    frame->msg_serial  = json_int64(root, "msgSerial", 0);
    frame->count       = (int)json_int64(root, "count", 0);
    frame->flags       = (int)json_int64(root, "flags", 0);
    frame->error_code  = 0;
    frame->error_message = NULL;
    frame->message_count = 0;

    /* Error object */
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err && cJSON_IsObject(err)) {
        frame->error_code    = (int)json_int64(err, "code", 0);
        frame->error_message = json_str(err, "message");
    }

    /* Messages array (action=MESSAGE) */
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (msgs && cJSON_IsArray(msgs) && frame->messages && frame->message_cap > 0) {
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, msgs) {
            if (frame->message_count >= frame->message_cap) break;
            ably_proto_message_t *pm = &frame->messages[frame->message_count++];
            pm->id            = json_str(m, "id");
            pm->client_id     = json_str(m, "clientId");
            pm->connection_id = json_str(m, "connectionId");
            pm->name          = json_str(m, "name");
            pm->data          = json_str(m, "data");
            pm->encoding      = json_str(m, "encoding");
            pm->timestamp     = json_int64(m, "timestamp", 0);
        }
    }

    return ABLY_OK;
}
