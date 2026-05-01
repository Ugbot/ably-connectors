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
#include "ably/ably_rest.h"
#include "delta/vcdiff.h"
#include "base64.h"
#include "cJSON.h"
#include "mpack.h"

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

/* Forward declaration — defined later in this file. */
static void flush_pending_queue(ably_channel_t *channel);

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
    if (channel->pres)           ably_mem_free(&channel->alloc, channel->pres);
    ably_mutex_destroy(&channel->sub_mutex);
    ably_mem_free(&channel->alloc, channel);
}

void ably_channel_on_frame(ably_channel_t *channel, const ably_proto_frame_t *frame)
{
    assert(channel != NULL);
    assert(frame   != NULL);

    switch (frame->action) {

    case ABLY_ACTION_ATTACHED:
        ABLY_LOG_I(&channel->log, "channel '%s' ATTACHED flags=%d", channel->name, frame->flags);
        ably_mutex_lock(&channel->sub_mutex);
        channel->reattach_pending = 0;
        channel->granted_modes    = frame->channel_modes;
        if (frame->channel_serial[0])
            memcpy(channel->channel_serial, frame->channel_serial,
                   sizeof(channel->channel_serial));
        memset(&channel->last_error, 0, sizeof(channel->last_error));
        set_state_locked(channel, ABLY_CHAN_ATTACHED, ABLY_OK);
        ably_mutex_unlock(&channel->sub_mutex);

        /* Flush messages queued while the channel was ATTACHING. */
        flush_pending_queue(channel);

        /* Begin presence SYNC if the server indicates the channel has members. */
        if ((frame->flags & ABLY_FLAG_HAS_PRESENCE) && channel->pres) {
            ably_presence_handle_sync(channel->pres, channel, frame);
        }
        /* Gap recovery: perform on non-resumed ATTACHED, OR when HAS_BACKLOG
         * is set (server detected a gap even in a resumed session). */
        if (!(frame->flags & ABLY_FLAG_RESUMED) ||
            (frame->flags & ABLY_FLAG_HAS_BACKLOG)) {
            if (channel->channel_serial[0])
                rt_recover_gap(channel->client, channel);
        }
        /* Re-enter own presence on non-resumed reconnect. */
        if (!(frame->flags & ABLY_FLAG_RESUMED)) {
            if (channel->pres)
                ably_presence_reenter_own(channel->pres, channel);
        }
        break;

    case ABLY_ACTION_DETACHED:
        ABLY_LOG_I(&channel->log, "channel '%s' DETACHED (code=%d)",
                   channel->name, frame->error_code);
        ably_mutex_lock(&channel->sub_mutex);
        if (frame->error_code != 0) {
            channel->last_error.ably_code = frame->error_code;
            snprintf(channel->last_error.message, sizeof(channel->last_error.message),
                     "%s", frame->error_message ? frame->error_message : "");
            set_state_locked(channel, ABLY_CHAN_FAILED, ABLY_ERR_PROTOCOL);
        } else {
            memset(&channel->last_error, 0, sizeof(channel->last_error));
            set_state_locked(channel, ABLY_CHAN_DETACHED, ABLY_OK);
        }
        ably_mutex_unlock(&channel->sub_mutex);
        break;

    case ABLY_ACTION_MESSAGE:
        /* Track channelSerial for gap recovery on reconnect. */
        if (frame->channel_serial[0])
            memcpy(channel->channel_serial, frame->channel_serial,
                   sizeof(channel->channel_serial));

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

            /* Resolved payload — may point into the delta decode buffer or
             * a stack-allocated base64 decode buffer. */
            const char *resolved_data = pm->data;

            /* If the message encoding is "base64", decode before dispatch.
             * The decoded bytes are stored in a stack buffer.  This handles
             * binary payloads that were base64-encoded by the server. */
            char b64_decoded_buf[ABLY_MAX_MESSAGE_DATA_LEN];
            if (pm->data && pm->encoding &&
                strstr(pm->encoding, "base64") != NULL &&
                (pm->delta_format == NULL)) {
                size_t b64_len  = strlen(pm->data);
                size_t out_len  = 0;
                if (ably_base64_decode((uint8_t *)b64_decoded_buf,
                                       ABLY_MAX_MESSAGE_DATA_LEN - 1,
                                       &out_len,
                                       pm->data, b64_len) == 0) {
                    b64_decoded_buf[out_len] = '\0';
                    resolved_data = b64_decoded_buf;
                }
            }

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

            /* Dispatch occupancy metrics if present. */
            if (pm->has_occupancy && channel->occupancy_listener) {
                channel->occupancy_listener(channel, &pm->occupancy,
                                            channel->occupancy_listener_user);
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

    case ABLY_ACTION_PRESENCE:
        /* Presence messages: track channelSerial, dispatch to presence module. */
        if (frame->channel_serial[0])
            memcpy(channel->channel_serial, frame->channel_serial,
                   sizeof(channel->channel_serial));
        if (channel->pres)
            ably_presence_handle_message(channel->pres, channel, frame);
        break;

    case ABLY_ACTION_SYNC:
        /* Presence SYNC: dispatch to presence module. */
        if (channel->pres)
            ably_presence_handle_sync(channel->pres, channel, frame);
        break;

    case ABLY_ACTION_ERROR:
        ABLY_LOG_E(&channel->log, "channel '%s' ERROR code=%d: %s",
                   channel->name, frame->error_code,
                   frame->error_message ? frame->error_message : "");
        ably_mutex_lock(&channel->sub_mutex);
        channel->last_error.ably_code = frame->error_code;
        snprintf(channel->last_error.message, sizeof(channel->last_error.message),
                 "%s", frame->error_message ? frame->error_message : "");
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
    /* Reset delta decode state so a stale source buffer is never applied to
     * a fresh delta sequence after reconnect. */
    channel->delta_buf_len[0] = 0;
    channel->delta_buf_len[1] = 0;
    channel->delta_buf_idx    = 0;
    channel->delta_last_id[0] = '\0';
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

void ably_channel_deliver_history(ably_channel_t *channel,
                                   const ably_message_t *msg)
{
    assert(channel != NULL);
    assert(msg     != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    for (int i = 0; i < ABLY_MAX_SUBSCRIBERS_PER_CHANNEL; i++) {
        ably_subscriber_t *sub = &channel->subscribers[i];
        if (sub->token == 0) continue;

        if (sub->name_filter[0] != '\0') {
            if (!msg->name || strcmp(msg->name, sub->name_filter) != 0) continue;
        }

        int             snap_token = sub->token;
        ably_message_cb cb         = sub->cb;
        void           *user_data  = sub->user_data;

        ably_mutex_unlock(&channel->sub_mutex);
        cb(channel, msg, user_data);
        ably_mutex_lock(&channel->sub_mutex);

        if (channel->subscribers[i].token != snap_token) break;
    }
    ably_mutex_unlock(&channel->sub_mutex);
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

    ably_attach_params_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.delta     = channel->delta_enabled;
    ap.rewind    = channel->rewind;
    ap.occupancy = channel->occupancy_listener ? 1 : 0;
    ap.channel_serial = channel->channel_serial[0] ? channel->channel_serial : NULL;

    char buf[ABLY_MAX_CHANNEL_NAME_LEN + 256];
    size_t n = ably_proto_encode_attach(buf, sizeof(buf), channel->name,
                                         &ap, channel->client->opts.encoding);
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

/* Flush pending messages enqueued during ATTACHING to the outbound ring.
 * Called from the service thread after ATTACHED is confirmed. */
static void flush_pending_queue(ably_channel_t *channel)
{
    while (channel->pending_tail != channel->pending_head) {
        int tail = channel->pending_tail;
        const char *name = channel->pending_queue[tail].name;
        const char *data = channel->pending_queue[tail].data;

        int64_t serial = rt_claim_msg_serial(channel->client);
        char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_MESSAGE_NAME_LEN +
                 ABLY_MAX_MESSAGE_DATA_LEN + 128];
        const char *cid = channel->client->client_id[0]
                          ? channel->client->client_id : NULL;
        size_t n = ably_proto_encode_publish(buf, sizeof(buf),
                                              channel->name,
                                              name[0] ? name : NULL,
                                              data[0] ? data : NULL,
                                              cid, NULL,
                                              serial,
                                              channel->client->opts.encoding);
        if (n > 0) rt_enqueue_frame(channel->client, buf, n);

        channel->pending_tail = (tail + 1) % ABLY_CHANNEL_PENDING_CAPACITY;
    }
}

ably_error_t ably_channel_publish(ably_channel_t *channel,
                                   const char     *name,
                                   const char     *data)
{
    assert(channel != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;

    /* While ATTACHING, queue the message for delivery after ATTACHED. */
    if (st == ABLY_CHAN_ATTACHING) {
        int next_head = (channel->pending_head + 1) % ABLY_CHANNEL_PENDING_CAPACITY;
        if (next_head == channel->pending_tail) {
            ably_mutex_unlock(&channel->sub_mutex);
            return ABLY_ERR_CAPACITY;
        }
        int slot = channel->pending_head;
        if (name) snprintf(channel->pending_queue[slot].name,
                           sizeof(channel->pending_queue[slot].name), "%s", name);
        else      channel->pending_queue[slot].name[0] = '\0';
        if (data) snprintf(channel->pending_queue[slot].data,
                           sizeof(channel->pending_queue[slot].data), "%s", data);
        else      channel->pending_queue[slot].data[0] = '\0';
        channel->pending_head = next_head;
        ably_mutex_unlock(&channel->sub_mutex);
        return ABLY_OK;
    }

    ably_mutex_unlock(&channel->sub_mutex);

    if (st != ABLY_CHAN_ATTACHED) return ABLY_ERR_STATE;

    /* Claim the next outbound message serial before encoding the frame.
     * The serial must be included in the frame (Ably protocol requirement). */
    int64_t msg_serial = rt_claim_msg_serial(channel->client);

    char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_MESSAGE_NAME_LEN +
             ABLY_MAX_MESSAGE_DATA_LEN + 128];
    const char *client_id = channel->client->client_id[0]
                            ? channel->client->client_id : NULL;
    size_t n = ably_proto_encode_publish(buf, sizeof(buf),
                                          channel->name, name, data,
                                          client_id, NULL,
                                          msg_serial,
                                          channel->client->opts.encoding);
    if (n == 0) return ABLY_ERR_INTERNAL;

    return rt_enqueue_frame(channel->client, buf, n);
}

ably_error_t ably_channel_publish_with_id(ably_channel_t *channel,
                                           const char     *name,
                                           const char     *data,
                                           const char     *id)
{
    assert(channel != NULL);

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;

    if (st == ABLY_CHAN_ATTACHING) {
        /* Queue without ID (pending queue doesn't store IDs) — fall through to
         * the ATTACHING enqueue in ably_channel_publish. */
        ably_mutex_unlock(&channel->sub_mutex);
        return ably_channel_publish(channel, name, data);
    }

    ably_mutex_unlock(&channel->sub_mutex);
    if (st != ABLY_CHAN_ATTACHED) return ABLY_ERR_STATE;

    int64_t msg_serial = rt_claim_msg_serial(channel->client);
    char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_MESSAGE_NAME_LEN +
             ABLY_MAX_MESSAGE_DATA_LEN + 256];
    const char *client_id = channel->client->client_id[0]
                            ? channel->client->client_id : NULL;
    size_t n = ably_proto_encode_publish(buf, sizeof(buf),
                                          channel->name, name, data,
                                          client_id, id,
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

void ably_channel_set_rewind(ably_channel_t *channel, int count)
{
    assert(channel != NULL);
    channel->rewind = (count > 0) ? count : 0;
}

void ably_channel_set_occupancy_listener(ably_channel_t      *channel,
                                          ably_occupancy_cb_t  cb,
                                          void                *user_data)
{
    assert(channel != NULL);
    channel->occupancy_listener      = cb;
    channel->occupancy_listener_user = user_data;
}

/* ---------------------------------------------------------------------------
 * Lazy presence state initialisation
 * --------------------------------------------------------------------------- */

static ably_presence_state_t *ensure_pres(ably_channel_t *ch)
{
    if (ch->pres) return ch->pres;
    ch->pres = ably_mem_malloc(&ch->alloc, sizeof(ably_presence_state_t));
    if (!ch->pres) return NULL;
    ably_presence_init(ch->pres);
    return ch->pres;
}

/* ---------------------------------------------------------------------------
 * Presence public API
 * --------------------------------------------------------------------------- */

ably_error_t ably_channel_presence_enter(ably_channel_t *channel,
                                          const char     *client_id,
                                          const char     *data)
{
    assert(channel != NULL);

    /* Use the client-level clientId as the default identity when the caller
     * passes NULL — consistent with the Ably JS SDK's identified-client model. */
    if (!client_id || !client_id[0])
        client_id = channel->client->client_id;
    if (!client_id || !client_id[0])
        return ABLY_ERR_INVALID_ARG; /* no identity on caller or client */

    if (ably_channel_state(channel) != ABLY_CHAN_ATTACHED)
        return ABLY_ERR_STATE;

    ably_presence_state_t *pres = ensure_pres(channel);
    if (!pres) return ABLY_ERR_NOMEM;

    snprintf(pres->own_client_id, sizeof(pres->own_client_id), "%s", client_id);
    if (data) snprintf(pres->own_data, sizeof(pres->own_data), "%s", data);
    else      pres->own_data[0] = '\0';
    pres->own_entered = 1;

    return ably_presence_reenter_own(pres, channel), ABLY_OK;
}

ably_error_t ably_channel_presence_leave(ably_channel_t *channel,
                                          const char     *data)
{
    assert(channel != NULL);

    if (ably_channel_state(channel) != ABLY_CHAN_ATTACHED)
        return ABLY_ERR_STATE;

    ably_presence_state_t *pres = channel->pres;
    if (!pres || !pres->own_entered) return ABLY_ERR_STATE;

    pres->own_entered = 0;

    /* Send LEAVE */
    char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_CLIENT_ID_LEN +
             ABLY_MAX_MESSAGE_DATA_LEN + 128];
    size_t n;
    if (channel->client->opts.encoding == ABLY_ENCODING_MSGPACK) {
        /* Use mpack */
        mpack_writer_t w;
        mpack_writer_init(&w, buf, sizeof(buf));
        mpack_build_map(&w);
        mpack_write_cstr(&w, "action");   mpack_write_int(&w, ABLY_ACTION_PRESENCE);
        mpack_write_cstr(&w, "channel");  mpack_write_cstr(&w, channel->name);
        mpack_write_cstr(&w, "presence"); mpack_start_array(&w, 1);
        mpack_build_map(&w);
        mpack_write_cstr(&w, "action");   mpack_write_int(&w, (int)ABLY_PRESENCE_LEAVE);
        mpack_write_cstr(&w, "clientId"); mpack_write_cstr(&w, pres->own_client_id);
        if (data && data[0]) { mpack_write_cstr(&w, "data"); mpack_write_cstr(&w, data); }
        mpack_complete_map(&w);
        mpack_finish_array(&w);
        mpack_complete_map(&w);
        n = (mpack_writer_destroy(&w) == mpack_ok) ? mpack_writer_buffer_used(&w) : 0;
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "action", ABLY_ACTION_PRESENCE);
        cJSON_AddStringToObject(root, "channel", channel->name);
        cJSON *pa = cJSON_AddArrayToObject(root, "presence");
        cJSON *m  = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "action", (int)ABLY_PRESENCE_LEAVE);
        cJSON_AddStringToObject(m, "clientId", pres->own_client_id);
        if (data && data[0]) cJSON_AddStringToObject(m, "data", data);
        cJSON_AddItemToArray(pa, m);
        char *s = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        n = 0;
        if (s) {
            n = strlen(s);
            if (n < sizeof(buf)) { memcpy(buf, s, n + 1); } else { n = 0; }
            cJSON_free(s);
        }
    }
    if (n == 0) return ABLY_ERR_INTERNAL;
    return rt_enqueue_frame(channel->client, buf, n);
}

ably_error_t ably_channel_presence_update(ably_channel_t *channel,
                                           const char     *data)
{
    assert(channel != NULL);

    if (ably_channel_state(channel) != ABLY_CHAN_ATTACHED)
        return ABLY_ERR_STATE;

    ably_presence_state_t *pres = channel->pres;
    if (!pres || !pres->own_entered) return ABLY_ERR_STATE;

    if (data) snprintf(pres->own_data, sizeof(pres->own_data), "%s", data);
    else      pres->own_data[0] = '\0';

    /* Send UPDATE */
    char buf[ABLY_MAX_CHANNEL_NAME_LEN + ABLY_MAX_CLIENT_ID_LEN +
             ABLY_MAX_MESSAGE_DATA_LEN + 128];
    size_t n;
    if (channel->client->opts.encoding == ABLY_ENCODING_MSGPACK) {
        mpack_writer_t w;
        mpack_writer_init(&w, buf, sizeof(buf));
        mpack_build_map(&w);
        mpack_write_cstr(&w, "action");   mpack_write_int(&w, ABLY_ACTION_PRESENCE);
        mpack_write_cstr(&w, "channel");  mpack_write_cstr(&w, channel->name);
        mpack_write_cstr(&w, "presence"); mpack_start_array(&w, 1);
        mpack_build_map(&w);
        mpack_write_cstr(&w, "action");   mpack_write_int(&w, (int)ABLY_PRESENCE_UPDATE);
        mpack_write_cstr(&w, "clientId"); mpack_write_cstr(&w, pres->own_client_id);
        if (data && data[0]) { mpack_write_cstr(&w, "data"); mpack_write_cstr(&w, data); }
        mpack_complete_map(&w);
        mpack_finish_array(&w);
        mpack_complete_map(&w);
        n = (mpack_writer_destroy(&w) == mpack_ok) ? mpack_writer_buffer_used(&w) : 0;
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "action", ABLY_ACTION_PRESENCE);
        cJSON_AddStringToObject(root, "channel", channel->name);
        cJSON *pa = cJSON_AddArrayToObject(root, "presence");
        cJSON *m  = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "action", (int)ABLY_PRESENCE_UPDATE);
        cJSON_AddStringToObject(m, "clientId", pres->own_client_id);
        if (data && data[0]) cJSON_AddStringToObject(m, "data", data);
        cJSON_AddItemToArray(pa, m);
        char *s = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        n = 0;
        if (s) {
            n = strlen(s);
            if (n < sizeof(buf)) { memcpy(buf, s, n + 1); } else { n = 0; }
            cJSON_free(s);
        }
    }
    if (n == 0) return ABLY_ERR_INTERNAL;
    return rt_enqueue_frame(channel->client, buf, n);
}

int ably_channel_presence_subscribe(ably_channel_t    *channel,
                                     ably_presence_cb_t cb,
                                     void              *user_data)
{
    assert(channel != NULL);
    assert(cb != NULL);

    ably_presence_state_t *pres = ensure_pres(channel);
    if (!pres) return 0;
    return ably_presence_subscribe(pres, cb, user_data);
}

void ably_channel_presence_unsubscribe(ably_channel_t *channel, int token)
{
    if (!channel || !channel->pres) return;
    ably_presence_unsubscribe(channel->pres, token);
}

int ably_channel_presence_get_members(ably_channel_t          *channel,
                                       ably_presence_message_t *out,
                                       int                      max,
                                       int                     *count_out)
{
    assert(channel != NULL);
    if (!channel->pres) {
        if (count_out) *count_out = 0;
        return 0;
    }
    return ably_presence_get_members(channel->pres, out, max, count_out);
}

/* ---------------------------------------------------------------------------
 * Error info, modes, options
 * --------------------------------------------------------------------------- */

const ably_error_info_t *ably_channel_last_error(const ably_channel_t *channel)
{
    assert(channel != NULL);
    return &channel->last_error;
}

void ably_channel_set_modes(ably_channel_t *channel, uint32_t modes)
{
    assert(channel != NULL);
    channel->channel_modes = modes;
}

uint32_t ably_channel_granted_modes(const ably_channel_t *channel)
{
    assert(channel != NULL);
    return channel->granted_modes;
}

ably_error_t ably_channel_set_options(ably_channel_t *channel,
                                       int             rewind,
                                       uint32_t        modes)
{
    assert(channel != NULL);

    channel->rewind        = (rewind > 0) ? rewind : 0;
    channel->channel_modes = modes;

    ably_mutex_lock(&channel->sub_mutex);
    ably_channel_state_t st = channel->state;
    ably_mutex_unlock(&channel->sub_mutex);

    if (st == ABLY_CHAN_ATTACHED || st == ABLY_CHAN_ATTACHING) {
        ably_error_t err = ably_channel_detach(channel);
        if (err != ABLY_OK) return err;
        err = ably_channel_attach(channel);
        if (err != ABLY_OK) return err;
    }
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Channel history (realtime: creates temp REST client, fetches history)
 * --------------------------------------------------------------------------- */

ably_error_t ably_channel_history(ably_channel_t       *channel,
                                   int                   limit,
                                   const char           *direction,
                                   const char           *from_serial,
                                   ably_history_page_t **page_out)
{
    assert(channel  != NULL);
    assert(page_out != NULL);

    *page_out = NULL;

    ably_rest_options_t ropts;
    ably_rest_options_init(&ropts);
    ropts.timeout_ms = 10000;

    ably_rest_client_t *rc = ably_rest_client_create(
        channel->client->api_key,
        &ropts,
        &channel->alloc);
    if (!rc) return ABLY_ERR_NOMEM;

    ably_error_t err = ably_rest_channel_history(rc, channel->name,
                                                   limit, direction,
                                                   from_serial, page_out);
    ably_rest_client_destroy(rc);
    return err;
}
