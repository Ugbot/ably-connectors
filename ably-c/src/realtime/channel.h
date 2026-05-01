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

#ifndef ABLY_CHANNEL_H
#define ABLY_CHANNEL_H

#include "ably/ably_realtime.h"
#include "ably/ably_types.h"
#include "protocol.h"
#include "presence.h"
#include "mutex.h"
#include "alloc.h"
#include "log.h"

/*
 * Subscriber entry.  Pre-allocated in the channel's fixed subscriber array.
 */
typedef struct {
    int             token;          /* 0 = slot is free */
    ably_message_cb cb;
    void           *user_data;
    char            name_filter[ABLY_MAX_MESSAGE_NAME_LEN]; /* "" = all */
} ably_subscriber_t;

/*
 * Internal channel structure.
 *
 * TigerStyle: all subscriber slots pre-allocated (no dynamic resize).
 * All fields protected by sub_mutex except name (immutable after create).
 */
struct ably_channel_s {
    char                 name[ABLY_MAX_CHANNEL_NAME_LEN];
    ably_channel_state_t state;

    /* Pre-allocated subscriber array. */
    ably_subscriber_t    subscribers[ABLY_MAX_SUBSCRIBERS_PER_CHANNEL];
    int                  next_token;       /* monotonically increasing */

    ably_mutex_t         sub_mutex;

    /* State-change callback (set before attach, not changed after). */
    ably_chan_state_cb   state_cb;
    void               *state_cb_user;

    /* Back-pointer to owning client (no ownership). */
    struct ably_rt_client_s *client;

    /* Set to 1 when the connection is lost; cleared after re-ATTACH sent. */
    int                  reattach_pending;

    /* Rewind: if > 0, includes params.rewind="N" in the ATTACH frame.
     * Must be set before ably_channel_attach(). */
    int                  rewind;

    /* channelSerial — updated on every ATTACHED/MESSAGE/PRESENCE frame.
     * Sent back in the next ATTACH frame to enable server-side gap detection. */
    char                 channel_serial[64];

    /* Occupancy listener — if non-NULL, adds params.occupancy="metrics.all" to ATTACH. */
    ably_occupancy_cb_t  occupancy_listener;
    void                *occupancy_listener_user;

    /* Channel modes requested by the client on ATTACH. */
    uint32_t             channel_modes;   /* 0 = all (default) */
    /* Channel modes granted by the server in the most recent ATTACHED frame. */
    uint32_t             granted_modes;   /* 0 = not specified by server */

    /* Last error from a server-sent channel error or DETACH with error. */
    ably_error_info_t    last_error;

    /*
     * Delta compression state.
     *
     * Enabled by ably_channel_enable_delta().  Two alternating buffers hold the
     * last decoded message payload so one can serve as the VCDIFF source while
     * the other receives the decoded output.  Only allocated when delta is
     * enabled (via channel->alloc).
     */
    int      delta_enabled;
    uint8_t *delta_bufs[2];          /* each ABLY_MAX_MESSAGE_DATA_LEN bytes */
    size_t   delta_buf_len[2];       /* bytes currently stored              */
    int      delta_buf_idx;          /* which buffer holds the latest data  */
    char     delta_last_id[128];     /* id of the last non-delta message    */

    ably_allocator_t     alloc;
    ably_log_ctx_t       log;

    /* Presence state — heap-allocated on first presence API call. */
    ably_presence_state_t *pres;

    /* Pending publish queue: messages enqueued while the channel is ATTACHING.
     * Flushed to the outbound ring buffer on ATTACHED.
     * Ring buffer: head = next write, tail = next read.
     * Empty when head == tail; full when (head+1)%cap == tail. */
#define ABLY_CHANNEL_PENDING_CAPACITY 32
    struct {
        char name[ABLY_MAX_MESSAGE_NAME_LEN];
        char data[ABLY_MAX_MESSAGE_DATA_LEN];
    }                    pending_queue[ABLY_CHANNEL_PENDING_CAPACITY];
    int                  pending_head;
    int                  pending_tail;
};

/* ---------------------------------------------------------------------------
 * Internal channel API (used by realtime_client.c)
 * --------------------------------------------------------------------------- */

/* Create and destroy a channel (called under client->chan_mutex). */
ably_channel_t *ably_channel_create(struct ably_rt_client_s *client,
                                     const char              *name,
                                     const ably_allocator_t  *alloc,
                                     const ably_log_ctx_t    *log);
void ably_channel_destroy(ably_channel_t *channel);

/* Called by the service thread when a protocol frame arrives for this channel. */
void ably_channel_on_frame(ably_channel_t *channel, const ably_proto_frame_t *frame);

/* Called when the connection drops. */
void ably_channel_on_disconnect(ably_channel_t *channel);

/* Called by realtime_client.c before sending ATTACH (sets state to ATTACHING). */
void ably_channel_set_attaching(ably_channel_t *channel);

/* Returns 1 if the channel should be re-attached on reconnect. */
int ably_channel_needs_reattach(const ably_channel_t *channel);

/* Deliver a single history message to all subscribers (used by gap recovery). */
void ably_channel_deliver_history(ably_channel_t *channel,
                                   const ably_message_t *msg);

#endif /* ABLY_CHANNEL_H */
