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
 * Presence module.
 *
 * Maintains the presence member set for a channel using a fixed-capacity pool +
 * hashmap (key = clientId).  Implements the SYNC state machine:
 *
 *   1. ATTACHED with HAS_PRESENCE flag → begin_sync(): mark all PRESENT stale.
 *   2. SYNC messages arrive → upsert members, clear stale.
 *   3. SYNC with empty cursor → end_sync(): synthesize LEAVE for all still-stale.
 *
 * No allocation on any hot path.  Pool is pre-allocated in the struct.
 *
 * PRESENCE and SYNC frames are dispatched here from channel.c.
 */

#ifndef ABLY_PRESENCE_H
#define ABLY_PRESENCE_H

#include "ably/ably_types.h"
#include "hashmap.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum presence subscribers per channel. */
#ifndef ABLY_MAX_PRESENCE_SUBSCRIBERS
#  define ABLY_MAX_PRESENCE_SUBSCRIBERS 16
#endif

/* Internal pool entry — wraps ably_presence_message_t with internal bookkeeping. */
typedef struct {
    ably_presence_message_t msg;
    int                     in_use;   /* 0 = free slot                        */
    int                     stale;    /* 1 = marked during ongoing SYNC       */
} ably_presence_member_t;

typedef struct {
    /* Member pool + hashmap.  key = clientId, value = &pool[i]. */
    ably_presence_member_t pool[ABLY_MAX_PRESENCE_MEMBERS];
    ably_hashmap_slot_t    slots[ABLY_MAX_PRESENCE_MEMBERS];
    ably_hashmap_t         map;

    /* SYNC state */
    int   syncing;              /* 1 while SYNC sequence in progress          */

    /* Own presence (for re-entry on non-resumed reconnect) */
    char  own_client_id[ABLY_MAX_CLIENT_ID_LEN];
    char  own_data[ABLY_MAX_MESSAGE_DATA_LEN];
    int   own_entered;

    /* Subscribers */
    ably_presence_cb_t  subs[ABLY_MAX_PRESENCE_SUBSCRIBERS];
    void               *sub_ud[ABLY_MAX_PRESENCE_SUBSCRIBERS];
    int                 sub_tokens[ABLY_MAX_PRESENCE_SUBSCRIBERS];
    int                 sub_count;
    int                 next_token;
} ably_presence_state_t;

/* Forward declaration — channel.h includes presence.h; presence.c uses channel_t. */
struct ably_channel_s;

/* ---------------------------------------------------------------------------
 * Internal API — called from channel.c
 * --------------------------------------------------------------------------- */

void ably_presence_init(ably_presence_state_t *pres);

/* Dispatch a PRESENCE frame (one or more presence messages). */
void ably_presence_handle_message(ably_presence_state_t    *pres,
                                   struct ably_channel_s    *ch,
                                   const ably_proto_frame_t *frame);

/* Dispatch a SYNC frame.
 * Begins sync on first call; ends and finalises on the last page
 * (detected by empty cursor in syncSerial). */
void ably_presence_handle_sync(ably_presence_state_t    *pres,
                                struct ably_channel_s    *ch,
                                const ably_proto_frame_t *frame);

/* Re-enter own presence after a non-resumed reconnect.
 * Enqueues a PRESENCE ENTER on the channel's send ring. */
void ably_presence_reenter_own(ably_presence_state_t *pres,
                                struct ably_channel_s *ch);

/* ---------------------------------------------------------------------------
 * Public API — called by application code via ably_realtime.h wrappers
 * --------------------------------------------------------------------------- */

/* Subscribe to presence events.  Returns a positive token, or 0 on failure. */
int ably_presence_subscribe(ably_presence_state_t *pres,
                             ably_presence_cb_t     cb,
                             void                  *user_data);

/* Unsubscribe by token. */
void ably_presence_unsubscribe(ably_presence_state_t *pres, int token);

/*
 * Copy PRESENT members into caller-supplied array.
 * Returns the number of entries written.  *count_out is set to the total
 * present member count (even if out[] was too small).
 */
int ably_presence_get_members(const ably_presence_state_t *pres,
                               ably_presence_message_t     *out,
                               int                          max,
                               int                         *count_out);

#ifdef __cplusplus
}
#endif
#endif /* ABLY_PRESENCE_H */
