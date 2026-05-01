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

#include "presence.h"
#include "channel.h"
#include "realtime_client.h"
#include "protocol.h"
#include "log.h"

#include "cJSON.h"
#include "mpack.h"

#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Pool helpers
 * --------------------------------------------------------------------------- */

static ably_presence_member_t *pool_alloc(ably_presence_state_t *pres)
{
    for (int i = 0; i < ABLY_MAX_PRESENCE_MEMBERS; i++) {
        if (!pres->pool[i].in_use) {
            memset(&pres->pool[i], 0, sizeof(pres->pool[i]));
            pres->pool[i].in_use = 1;
            return &pres->pool[i];
        }
    }
    return NULL;  /* pool full */
}

static void pool_free(ably_presence_state_t *pres, ably_presence_member_t *m)
{
    (void)pres;
    m->in_use = 0;
}

/* ---------------------------------------------------------------------------
 * Subscriber dispatch
 * --------------------------------------------------------------------------- */

static void notify_subscribers(ably_presence_state_t       *pres,
                                struct ably_channel_s       *ch,
                                const ably_presence_message_t *msg)
{
    for (int i = 0; i < ABLY_MAX_PRESENCE_SUBSCRIBERS; i++) {
        if (pres->sub_tokens[i] > 0 && pres->subs[i]) {
            pres->subs[i](ch, msg, pres->sub_ud[i]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Upsert / remove member
 * --------------------------------------------------------------------------- */

static void upsert_member(ably_presence_state_t         *pres,
                           struct ably_channel_s         *ch,
                           const ably_presence_message_t *msg,
                           int                            clear_stale)
{
    ably_presence_member_t *existing =
        (ably_presence_member_t *)ably_hashmap_get(&pres->map, msg->client_id);

    if (existing) {
        existing->msg    = *msg;
        if (clear_stale) existing->stale = 0;
    } else {
        ably_presence_member_t *m = pool_alloc(pres);
        if (!m) {
            ABLY_LOG_E(&ch->log, "presence: pool full, cannot add member '%s'",
                       msg->client_id);
            return;
        }
        m->msg   = *msg;
        m->stale = 0;
        if (ably_hashmap_put(&pres->map, msg->client_id, m) != 0) {
            ABLY_LOG_E(&ch->log, "presence: hashmap full, cannot add member '%s'",
                       msg->client_id);
            pool_free(pres, m);
            return;
        }
    }
    /* Callers are responsible for notification. */
}

static void remove_member(ably_presence_state_t         *pres,
                           const ably_presence_message_t *msg)
{
    ably_presence_member_t *existing =
        (ably_presence_member_t *)ably_hashmap_get(&pres->map, msg->client_id);

    if (existing) {
        pool_free(pres, existing);
        ably_hashmap_remove(&pres->map, msg->client_id);
    }
    /* Callers are responsible for notification. */
}

/* ---------------------------------------------------------------------------
 * SYNC state machine
 * --------------------------------------------------------------------------- */

/* Mark all current PRESENT members stale. */
static void begin_sync(ably_presence_state_t *pres)
{
    for (int i = 0; i < ABLY_MAX_PRESENCE_MEMBERS; i++) {
        if (pres->pool[i].in_use &&
            pres->pool[i].msg.action == ABLY_PRESENCE_PRESENT) {
            pres->pool[i].stale = 1;
        }
    }
    pres->syncing = 1;
}

/* After the last SYNC page: synthesize LEAVE for all still-stale members. */
static void end_sync(ably_presence_state_t *pres, struct ably_channel_s *ch)
{
    for (int i = 0; i < ABLY_MAX_PRESENCE_MEMBERS; i++) {
        if (pres->pool[i].in_use && pres->pool[i].stale) {
            ably_presence_message_t leave = pres->pool[i].msg;
            leave.action = ABLY_PRESENCE_LEAVE;

            pool_free(pres, &pres->pool[i]);
            ably_hashmap_remove(&pres->map, leave.client_id);

            notify_subscribers(pres, ch, &leave);
        }
    }
    pres->syncing = 0;
}

/* ---------------------------------------------------------------------------
 * Encode a PRESENCE frame for outbound messages (ENTER/LEAVE/UPDATE).
 * Returns bytes written into buf, or 0 on error.
 * --------------------------------------------------------------------------- */

static size_t encode_presence_json(char *buf, size_t len,
                                    const char *channel,
                                    ably_presence_action_t action,
                                    const char *client_id,
                                    const char *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return 0;

    cJSON_AddNumberToObject(root, "action", ABLY_ACTION_PRESENCE);
    if (channel) cJSON_AddStringToObject(root, "channel", channel);

    cJSON *pres_arr = cJSON_AddArrayToObject(root, "presence");
    cJSON *msg      = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "action", (int)action);
    if (client_id) cJSON_AddStringToObject(msg, "clientId", client_id);
    if (data)      cJSON_AddStringToObject(msg, "data",     data);
    cJSON_AddItemToArray(pres_arr, msg);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return 0;

    size_t slen = strlen(s);
    if (slen >= len) { cJSON_free(s); return 0; }
    memcpy(buf, s, slen + 1);
    cJSON_free(s);
    return slen;
}

static size_t encode_presence_msgpack(uint8_t *buf, size_t len,
                                       const char *channel,
                                       ably_presence_action_t action,
                                       const char *client_id,
                                       const char *data)
{
    mpack_writer_t w;
    mpack_writer_init(&w, (char *)buf, len);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "action");
    mpack_write_int(&w, ABLY_ACTION_PRESENCE);
    if (channel) {
        mpack_write_cstr(&w, "channel");
        mpack_write_cstr(&w, channel);
    }
    mpack_write_cstr(&w, "presence");
    mpack_start_array(&w, 1);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "action");
    mpack_write_int(&w, (int)action);
    if (client_id) {
        mpack_write_cstr(&w, "clientId");
        mpack_write_cstr(&w, client_id);
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

static ably_error_t send_presence(struct ably_channel_s  *ch,
                                   ably_presence_action_t  action,
                                   const char             *client_id,
                                   const char             *data)
{
    char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_CLIENT_ID_LEN +
             ABLY_MAX_MESSAGE_DATA_LEN + 128];
    size_t n;

    if (ch->client->opts.encoding == ABLY_ENCODING_MSGPACK) {
        n = encode_presence_msgpack((uint8_t *)buf, sizeof(buf),
                                     ch->name, action, client_id, data);
    } else {
        n = encode_presence_json(buf, sizeof(buf),
                                  ch->name, action, client_id, data);
    }

    if (n == 0) return ABLY_ERR_INTERNAL;
    return rt_enqueue_frame(ch->client, buf, n);
}

/* ---------------------------------------------------------------------------
 * Public API implementation
 * --------------------------------------------------------------------------- */

void ably_presence_init(ably_presence_state_t *pres)
{
    assert(pres != NULL);
    memset(pres, 0, sizeof(*pres));
    ably_hashmap_init(&pres->map, pres->slots, ABLY_MAX_PRESENCE_MEMBERS);
    pres->next_token = 1;
}

void ably_presence_handle_message(ably_presence_state_t    *pres,
                                   struct ably_channel_s    *ch,
                                   const ably_proto_frame_t *frame)
{
    assert(pres != NULL);
    assert(frame != NULL);

    for (size_t i = 0; i < frame->presence_count; i++) {
        const ably_presence_message_t *msg = &frame->presence_msgs[i];

        switch (msg->action) {
        case ABLY_PRESENCE_ENTER:
        case ABLY_PRESENCE_PRESENT:
        case ABLY_PRESENCE_UPDATE: {
            ably_presence_message_t m = *msg;
            m.action = ABLY_PRESENCE_PRESENT;  /* normalise to PRESENT in the map */
            upsert_member(pres, ch, &m, 1);
            notify_subscribers(pres, ch, msg); /* notify with original action */
            break;
        }
        case ABLY_PRESENCE_ABSENT:
        case ABLY_PRESENCE_LEAVE:
            remove_member(pres, msg);
            notify_subscribers(pres, ch, msg);
            break;
        default:
            break;
        }
    }
}

void ably_presence_handle_sync(ably_presence_state_t    *pres,
                                struct ably_channel_s    *ch,
                                const ably_proto_frame_t *frame)
{
    assert(pres != NULL);
    assert(frame != NULL);

    /* First SYNC page (or a fresh sync): begin_sync marks all stale. */
    if (!pres->syncing)
        begin_sync(pres);

    /* Upsert members from this SYNC page (clear stale as we see each one). */
    for (size_t i = 0; i < frame->presence_count; i++) {
        const ably_presence_message_t *msg = &frame->presence_msgs[i];
        ably_presence_message_t m = *msg;
        m.action = ABLY_PRESENCE_PRESENT;
        upsert_member(pres, ch, &m, 1);
        /* Notify subscribers with the original SYNC message. */
        notify_subscribers(pres, ch, msg);
    }

    /* Detect last SYNC page: cursor part of syncSerial is empty.
     * syncSerial format: "prefix:cursor" — if cursor is absent or empty,
     * this is the last page. */
    const char *serial = frame->sync_serial;
    int is_last_page = 1;
    if (serial && serial[0]) {
        const char *colon = strchr(serial, ':');
        if (colon && colon[1] != '\0')
            is_last_page = 0;  /* cursor present — more pages follow */
    }

    if (is_last_page)
        end_sync(pres, ch);
}

void ably_presence_reenter_own(ably_presence_state_t *pres,
                                struct ably_channel_s *ch)
{
    if (!pres->own_entered || pres->own_client_id[0] == '\0') return;

    send_presence(ch, ABLY_PRESENCE_ENTER,
                  pres->own_client_id, pres->own_data);
}

int ably_presence_subscribe(ably_presence_state_t *pres,
                             ably_presence_cb_t     cb,
                             void                  *user_data)
{
    assert(pres != NULL);
    assert(cb   != NULL);

    for (int i = 0; i < ABLY_MAX_PRESENCE_SUBSCRIBERS; i++) {
        if (pres->sub_tokens[i] == 0) {
            int token = pres->next_token++;
            if (pres->next_token <= 0) pres->next_token = 1;
            pres->subs[i]       = cb;
            pres->sub_ud[i]     = user_data;
            pres->sub_tokens[i] = token;
            pres->sub_count++;
            return token;
        }
    }
    return 0;  /* no free slot */
}

void ably_presence_unsubscribe(ably_presence_state_t *pres, int token)
{
    if (!pres || token <= 0) return;
    for (int i = 0; i < ABLY_MAX_PRESENCE_SUBSCRIBERS; i++) {
        if (pres->sub_tokens[i] == token) {
            pres->subs[i]       = NULL;
            pres->sub_ud[i]     = NULL;
            pres->sub_tokens[i] = 0;
            pres->sub_count--;
            return;
        }
    }
}

int ably_presence_get_members(const ably_presence_state_t *pres,
                               ably_presence_message_t     *out,
                               int                          max,
                               int                         *count_out)
{
    assert(pres != NULL);
    int total = 0;
    int written = 0;

    for (int i = 0; i < ABLY_MAX_PRESENCE_MEMBERS; i++) {
        if (pres->pool[i].in_use &&
            pres->pool[i].msg.action == ABLY_PRESENCE_PRESENT) {
            total++;
            if (out && written < max) {
                out[written++] = pres->pool[i].msg;
            }
        }
    }

    if (count_out) *count_out = total;
    return written;
}
