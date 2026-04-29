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
 * Channel state machine and subscriber dispatch.
 *
 * TigerStyle:
 *   - All subscriber slots pre-allocated in the channel struct (no resize).
 *   - sub_mutex protects all mutable fields except 'name' (immutable post-create).
 *   - No allocation on any hot path (subscribe/publish/dispatch).
 *   - On_frame is called from the service thread only; publish from app threads.
 */

#include "channel.h"
#include "realtime_client.h"
#include "protocol.h"

#include "ably/ably_realtime.h"
#include "delta/vcdiff.h"
#include "base64.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/* Set channel state and invoke the state callback if installed.
 * Caller must hold ch->sub_mutex. */
static void set_state_locked(ably_channel_t    *ch,
                              ably_channel_state_t new_state,
                              ably_error_t         reason)
{
    ably_channel_state_t old = ch->state;
    if (old == new_state) return;
    ch->state = new_state;
    if (ch->state_cb) {
        ch->state_cb(ch, new_state, old, reason, ch->state_cb_user);
    }
}

/* ---------------------------------------------------------------------------
 * Internal channel API
 * --------------------------------------------------------------------------- */

ably_channel_t *ably_channel_create(struct ably_rt_client_s *client,
                                     const char              *name,
                                     const ably_allocator_t  *alloc,
                                     const ably_log_ctx_t    *log)
{
    assert(client != NULL);
    assert(name   != NULL);
    assert(alloc  != NULL);

    ably_channel_t *ch = ably_mem_malloc(alloc, sizeof(*ch));
    if (!ch) return NULL;

    memset(ch, 0, sizeof(*ch));
    snprintf(ch->name, sizeof(ch->name), "%s", name);
    ch->state          = ABLY_CHAN_INITIALIZED;
    ch->next_token     = 1;  /* 0 is "slot free" sentinel */
    ch->client         = client;
    ch->reattach_pending = 0;
    ch->alloc          = *alloc;
    if (log) ch->log   = *log;

    ably_mutex_init(&ch->sub_mutex);

    ABLY_LOG_D(&ch->log, "channel '%s' created", ch->name);
    return ch;
}

void ably_channel_destroy(ably_channel_t *channel)
{
    if (!channel) return;
    if (channel->delta_bufs[0]) ably_mem_free(&channel->alloc, channel->delta_bufs[0]);
    if (channel->delta_bufs[1]) ably_mem_free(&channel->alloc, channel->delta_bufs[1]);
    ably_mutex_destroy(&channel->sub_mutex);
    ably_mem_free(&channel->alloc, channel);
}

void ably_channel_on_frame(ably_channel_t *channel, const ably_proto_frame_t *frame)
{
    assert(channel != NULL);
    assert(frame   != NULL);

    switch (frame->action) {

    case ABLY_ACTION_ATTACHED:
        ABLY_LOG_I(&channel->log, "channel '%s' ATTACHED", channel->name);
        ably_mutex_lock(&channel->sub_mutex);
        channel->reattach_pending = 0;
        set_state_locked(channel, ABLY_CHAN_ATTACHED, ABLY_OK);
        ably_mutex_unlock(&channel->sub_mutex);
        break;

    case ABLY_ACTION_DETACHED:
        ABLY_LOG_I(&channel->log, "channel '%s' DETACHED (code=%d)",
                   channel->name, frame->error_code);
        ably_mutex_lock(&channel->sub_mutex);
        set_state_locked(channel, ABLY_CHAN_DETACHED, ABLY_OK);
        ably_mutex_unlock(&channel->sub_mutex);
        break;

    case ABLY_ACTION_MESSAGE:
        /* Dispatch to matching subscribers.
         *
         * For delta-enabled channels, VCDIFF-encoded messages are decoded
         * before dispatch.  The decoded bytes replace pm->data for the
         * duration of the dispatch only; they are stored in the channel's
         * alternating delta buffers (no extra allocation on the hot path).
         *
         * We release sub_mutex while invoking each callback to prevent
         * deadlock if the callback calls back into the channel API.
         * After re-acquiring, we verify the slot token still matches. */
        for (size_t m = 0; m < frame->message_count; m++) {
            const ably_proto_message_t *pm = &frame->messages[m];

            /* Resolved payload — may point into the delta decode buffer. */
            const char *resolved_data = pm->data;

            if (channel->delta_enabled && pm->delta_format &&
                strcmp(pm->delta_format, "vcdiff") == 0) {
                /* Delta message: base64-decode the data, apply VCDIFF patch. */
                const char *b64 = pm->data ? pm->data : "";
                size_t b64_len  = b64 ? strlen(b64) : 0;

                /* Workspace: decode into the inactive buffer. */
                int src_idx = channel->delta_buf_idx;
                int dst_idx = src_idx ^ 1;

                /* Base64-decode the raw VCDIFF bytes into a stack scratch buf.
                 * Maximum decoded size is 3/4 of the base64 string. */
                size_t max_delta = ably_base64_decode_max_len(b64_len);
                if (max_delta > ABLY_MAX_MESSAGE_DATA_LEN) {
                    ABLY_LOG_E(&channel->log,
                               "channel '%s': delta too large (%zu), dropping",
                               channel->name, max_delta);
                    continue;
                }

                /* Use the dst delta buffer as a scratch area for the raw VCDIFF
                 * bytes (first half), then decode the result into the same buffer
                 * (it's fine because VCDIFF reads source, writes output). */
                uint8_t *scratch = channel->delta_bufs[dst_idx];
                size_t   raw_len = 0;
                if (ably_base64_decode(scratch, ABLY_MAX_MESSAGE_DATA_LEN,
                                        &raw_len, b64, b64_len) != 0) {
                    ABLY_LOG_E(&channel->log,
                               "channel '%s': base64 decode failed for delta",
                               channel->name);
                    continue;
                }

                /* Copy the raw VCDIFF bytes into the channel's temporary stack
                 * buffer so we can reuse dst_buf for the output. */
                uint8_t vcdiff_tmp[ABLY_MAX_MESSAGE_DATA_LEN];
                if (raw_len > sizeof(vcdiff_tmp)) {
                    ABLY_LOG_E(&channel->log,
                               "channel '%s': VCDIFF delta too large", channel->name);
                    continue;
                }
                memcpy(vcdiff_tmp, scratch, raw_len);

                const uint8_t *source     = channel->delta_bufs[src_idx];
                size_t         source_len = channel->delta_buf_len[src_idx];
                uint8_t       *out_buf    = channel->delta_bufs[dst_idx];
                size_t         out_len    = ABLY_MAX_MESSAGE_DATA_LEN - 1; /* -1 for NUL */

                ably_vcdiff_error_t verr = ably_vcdiff_decode(
                    source, source_len,
                    vcdiff_tmp, raw_len,
                    out_buf, &out_len);

                if (verr != ABLY_VCDIFF_OK) {
                    ABLY_LOG_E(&channel->log,
                               "channel '%s': VCDIFF decode failed (%d)",
                               channel->name, (int)verr);
                    continue;
                }

                /* NUL-terminate so it can be used as a C string. */
                out_buf[out_len] = '\0';
                channel->delta_buf_len[dst_idx] = out_len;
                channel->delta_buf_idx = dst_idx;
                resolved_data = (const char *)out_buf;

            } else if (channel->delta_enabled && pm->data) {
                /* Full (non-delta) message: store raw bytes as new base. */
                int dst_idx = channel->delta_buf_idx ^ 1;
                size_t data_len = strlen(pm->data);
                if (data_len < ABLY_MAX_MESSAGE_DATA_LEN) {
                    memcpy(channel->delta_bufs[dst_idx], pm->data, data_len);
                    channel->delta_buf_len[dst_idx] = data_len;
                    channel->delta_buf_idx = dst_idx;
                }
                if (pm->id) {
                    snprintf(channel->delta_last_id,
                             sizeof(channel->delta_last_id), "%s", pm->id);
                }
                resolved_data = pm->data;
            }

            /* Dispatch to subscribers */
            ably_mutex_lock(&channel->sub_mutex);
            for (int i = 0; i < ABLY_MAX_SUBSCRIBERS_PER_CHANNEL; i++) {
                ably_subscriber_t *sub = &channel->subscribers[i];
                if (sub->token == 0) continue;

                /* Apply name filter if set. */
                if (sub->name_filter[0] != '\0') {
                    if (!pm->name || strcmp(pm->name, sub->name_filter) != 0)
                        continue;
                }

                ably_message_t msg;
                msg.id        = pm->id;
                msg.name      = pm->name;
                msg.data      = resolved_data;
                msg.client_id = pm->client_id;
                msg.timestamp = pm->timestamp;

                int             snap_token = sub->token;
                ably_message_cb cb         = sub->cb;
                void           *user_data  = sub->user_data;

                ably_mutex_unlock(&channel->sub_mutex);
                cb(channel, &msg, user_data);
                ably_mutex_lock(&channel->sub_mutex);

                if (channel->subscribers[i].token != snap_token) break;
            }
            ably_mutex_unlock(&channel->sub_mutex);
        }
        break;

    case ABLY_ACTION_ERROR:
        ABLY_LOG_E(&channel->log, "channel '%s' ERROR code=%d: %s",
                   channel->name, frame->error_code,
                   frame->error_message ? frame->error_message : "");
        ably_mutex_lock(&channel->sub_mutex);
        set_state_locked(channel, ABLY_CHAN_FAILED, ABLY_ERR_PROTOCOL);
        ably_mutex_unlock(&channel->sub_mutex);
        break;

    default:
        break;
    }
}

void ably_channel_on_disconnect(ably_channel_t *channel)
{
    assert(channel != NULL);
    ably_mutex_lock(&channel->sub_mutex);
    if (channel->state == ABLY_CHAN_ATTACHED ||
        channel->state == ABLY_CHAN_ATTACHING) {
        channel->reattach_pending = 1;
    }
    set_state_locked(channel, ABLY_CHAN_DETACHED, ABLY_ERR_NETWORK);
    ably_mutex_unlock(&channel->sub_mutex);
}

void ably_channel_set_attaching(ably_channel_t *channel)
{
    assert(channel != NULL);
    ably_mutex_lock(&channel->sub_mutex);
    channel->reattach_pending = 0;
    set_state_locked(channel, ABLY_CHAN_ATTACHING, ABLY_OK);
    ably_mutex_unlock(&channel->sub_mutex);
}

int ably_channel_needs_reattach(const ably_channel_t *channel)
{
    assert(channel != NULL);
    return channel->reattach_pending;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

ably_error_t ably_channel_enable_delta(ably_channel_t *channel)
{
    assert(channel != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;
    ably_mutex_unlock(&channel->sub_mutex);

    if (st == ABLY_CHAN_ATTACHED || st == ABLY_CHAN_ATTACHING)
        return ABLY_ERR_STATE;

    if (channel->delta_enabled) return ABLY_OK;  /* idempotent */

    size_t buf_size = ABLY_MAX_MESSAGE_DATA_LEN;
    channel->delta_bufs[0] = ably_mem_malloc(&channel->alloc, buf_size);
    channel->delta_bufs[1] = ably_mem_malloc(&channel->alloc, buf_size);
    if (!channel->delta_bufs[0] || !channel->delta_bufs[1]) {
        ably_mem_free(&channel->alloc, channel->delta_bufs[0]);
        ably_mem_free(&channel->alloc, channel->delta_bufs[1]);
        channel->delta_bufs[0] = channel->delta_bufs[1] = NULL;
        return ABLY_ERR_NOMEM;
    }
    channel->delta_buf_len[0] = 0;
    channel->delta_buf_len[1] = 0;
    channel->delta_buf_idx    = 0;
    channel->delta_last_id[0] = '\0';
    channel->delta_enabled    = 1;
    return ABLY_OK;
}

void ably_channel_set_state_cb(ably_channel_t    *channel,
                                ably_chan_state_cb  cb,
                                void              *user_data)
{
    assert(channel != NULL);
    ably_mutex_lock(&channel->sub_mutex);
    channel->state_cb      = cb;
    channel->state_cb_user = user_data;
    ably_mutex_unlock(&channel->sub_mutex);
}

ably_error_t ably_channel_attach(ably_channel_t *channel)
{
    assert(channel != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;
    ably_mutex_unlock(&channel->sub_mutex);

    if (st == ABLY_CHAN_ATTACHED || st == ABLY_CHAN_ATTACHING)
        return ABLY_OK;  /* idempotent */

    /* Check connection state before sending. */
    ably_connection_state_t conn_state = ably_rt_client_state(channel->client);
    if (conn_state != ABLY_CONN_CONNECTED) return ABLY_ERR_STATE;

    char buf[ABLY_MAX_CHANNEL_NAME_LEN + 64];
    size_t n = ably_proto_encode_attach(buf, sizeof(buf), channel->name,
                                         channel->delta_enabled,
                                         channel->client->opts.encoding);
    if (n == 0) return ABLY_ERR_INTERNAL;

    ably_error_t err = rt_enqueue_frame(channel->client, buf, n);
    if (err != ABLY_OK) return err;

    ably_mutex_lock(&channel->sub_mutex);
    set_state_locked(channel, ABLY_CHAN_ATTACHING, ABLY_OK);
    ably_mutex_unlock(&channel->sub_mutex);

    return ABLY_OK;
}

ably_error_t ably_channel_detach(ably_channel_t *channel)
{
    assert(channel != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;
    ably_mutex_unlock(&channel->sub_mutex);

    if (st != ABLY_CHAN_ATTACHED && st != ABLY_CHAN_ATTACHING)
        return ABLY_ERR_STATE;

    char buf[ABLY_MAX_CHANNEL_NAME_LEN + 32];
    size_t n = ably_proto_encode_detach(buf, sizeof(buf), channel->name,
                                         channel->client->opts.encoding);
    if (n == 0) return ABLY_ERR_INTERNAL;

    ably_error_t err = rt_enqueue_frame(channel->client, buf, n);
    if (err != ABLY_OK) return err;

    ably_mutex_lock(&channel->sub_mutex);
    set_state_locked(channel, ABLY_CHAN_DETACHING, ABLY_OK);
    ably_mutex_unlock(&channel->sub_mutex);

    return ABLY_OK;
}

int ably_channel_subscribe(ably_channel_t  *channel,
                            const char      *name_filter,
                            ably_message_cb  cb,
                            void            *user_data)
{
    assert(channel  != NULL);
    assert(cb       != NULL);

    ably_mutex_lock(&channel->sub_mutex);

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < ABLY_MAX_SUBSCRIBERS_PER_CHANNEL; i++) {
        if (channel->subscribers[i].token == 0) { slot = i; break; }
    }
    if (slot < 0) {
        ably_mutex_unlock(&channel->sub_mutex);
        return (int)ABLY_ERR_CAPACITY;
    }

    int token = channel->next_token++;
    if (channel->next_token <= 0) channel->next_token = 1;  /* wrap, skip 0 */

    ably_subscriber_t *sub = &channel->subscribers[slot];
    sub->token     = token;
    sub->cb        = cb;
    sub->user_data = user_data;

    if (name_filter && name_filter[0] != '\0') {
        snprintf(sub->name_filter, sizeof(sub->name_filter), "%s", name_filter);
    } else {
        sub->name_filter[0] = '\0';
    }

    ably_mutex_unlock(&channel->sub_mutex);
    return token;
}

ably_error_t ably_channel_unsubscribe(ably_channel_t *channel, int token)
{
    assert(channel != NULL);
    if (token <= 0) return ABLY_ERR_INVALID_ARG;

    ably_mutex_lock(&channel->sub_mutex);
    for (int i = 0; i < ABLY_MAX_SUBSCRIBERS_PER_CHANNEL; i++) {
        if (channel->subscribers[i].token == token) {
            memset(&channel->subscribers[i], 0, sizeof(channel->subscribers[i]));
            ably_mutex_unlock(&channel->sub_mutex);
            return ABLY_OK;
        }
    }
    ably_mutex_unlock(&channel->sub_mutex);
    return ABLY_ERR_INVALID_ARG;
}

ably_error_t ably_channel_publish(ably_channel_t *channel,
                                   const char     *name,
                                   const char     *data)
{
    assert(channel != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;
    ably_mutex_unlock(&channel->sub_mutex);

    if (st != ABLY_CHAN_ATTACHED) return ABLY_ERR_STATE;

    /* Claim the next outbound message serial before encoding the frame.
     * The serial must be included in the frame (Ably protocol requirement). */
    int64_t msg_serial = rt_claim_msg_serial(channel->client);

    char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_MESSAGE_NAME_LEN + ABLY_MAX_MESSAGE_DATA_LEN + 64];
    size_t n = ably_proto_encode_publish(buf, sizeof(buf),
                                          channel->name, name, data,
                                          msg_serial,
                                          channel->client->opts.encoding);
    if (n == 0) return ABLY_ERR_INTERNAL;

    return rt_enqueue_frame(channel->client, buf, n);
}

ably_channel_state_t ably_channel_state(const ably_channel_t *channel)
{
    assert(channel != NULL);
    ably_mutex_lock((ably_mutex_t *)&channel->sub_mutex);
    ably_channel_state_t s = channel->state;
    ably_mutex_unlock((ably_mutex_t *)&channel->sub_mutex);
    return s;
}

const char *ably_channel_name(const ably_channel_t *channel)
{
    assert(channel != NULL);
    return channel->name;
}
