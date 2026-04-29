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

    ably_allocator_t     alloc;
    ably_log_ctx_t       log;
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

#endif /* ABLY_CHANNEL_H */
