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

#ifndef ABLY_REALTIME_CLIENT_H
#define ABLY_REALTIME_CLIENT_H

#include "ably/ably_realtime.h"
#include "ably/ably_types.h"
#include "ws/ws_client.h"
#include "realtime/protocol.h"
#include "mutex.h"
#include "alloc.h"
#include "log.h"

/* Forward declaration — channel.h includes this header. */
struct ably_channel_s;

/*
 * Outbound frame for the ring buffer.
 * Payload is a pre-serialised JSON or MessagePack string.
 */
#define ABLY_FRAME_PAYLOAD_MAX (64 * 1024)

typedef struct {
    char   payload[ABLY_FRAME_PAYLOAD_MAX];
    size_t payload_len;
} ably_outbound_frame_t;

/*
 * Internal real-time client structure.
 *
 * TigerStyle: all channel handles, subscriber arrays, and outbound frames
 * are pre-allocated at create time.  No allocation occurs on any hot path.
 */
struct ably_rt_client_s {
    /* Connection state (protected by state_mutex) */
    ably_connection_state_t  state;
    ably_mutex_t             state_mutex;
    ably_cond_t              state_cond;

    /* WebSocket */
    ably_ws_client_t        *ws;

    /* Service thread */
    ably_thread_t            service_thread;
    int                      service_thread_running;
    int                      close_requested;       /* atomic-ish: written under state_mutex */

    /* Outbound ring buffer (send queue).
     * Producer: application threads (ably_channel_publish).
     * Consumer: service thread (drains in recv loop).
     * Protected by send_mutex.
     *
     * Ring indices: head = next write position, tail = next read position.
     * Empty: head == tail.  Full: (head+1) % cap == tail.
     */
    ably_outbound_frame_t   *send_ring;              /* ABLY_SEND_RING_CAPACITY frames */
    volatile size_t          send_head;              /* write index (producer) */
    volatile size_t          send_tail;              /* read index (consumer) */
    ably_mutex_t             send_mutex;
    ably_cond_t              send_cond;

    /* Channel map (protected by chan_mutex) */
    struct ably_channel_s   *channels[ABLY_MAX_CHANNELS];
    size_t                   channel_count;
    ably_mutex_t             chan_mutex;

    /* Reconnection state */
    int                      reconnect_attempt;

    /* Outbound message serial — increments with every client-originated publish.
     * Resets to 0 on each new connection.  Protected by send_mutex. */
    int64_t                  outbound_msg_serial;

    /* Server-assigned connection identity (service thread only — no locking needed).
     * Populated from the CONNECTED frame; cleared on each new connect attempt. */
    char                     connection_id[64];   /* e.g. "abc123"              */
    char                     connection_key[256]; /* resume key for reconnect   */

    /* Heartbeat watchdog (service thread only — no locking needed).
     * Updated on HEARTBEAT or CONNECTED receipt; checked in the event loop. */
    int64_t                  last_activity_ms;  /* monotonic ms, 0 = not yet set */

    /* Protocol decode context (service thread only — no locking needed) */
    ably_proto_message_t     decode_msgs[32];   /* message array for inbound frames */
    ably_proto_frame_t       decode_frame;

    /* Options */
    ably_rt_options_t        opts;
    char                     api_key[ABLY_MAX_KEY_LEN];
    char                     ws_path[512];           /* e.g. "/?v=2&key=..." */

    /* Callbacks */
    ably_conn_state_cb       conn_state_cb;
    void                    *conn_state_user;

    ably_allocator_t         alloc;
    ably_log_ctx_t           log;
};

/* Transition to a new connection state (call with state_mutex held). */
void rt_set_state_locked(struct ably_rt_client_s *client,
                          ably_connection_state_t  new_state,
                          ably_error_t             reason);

/* Enqueue a pre-serialised frame for the service thread to send.
 * Called by channel.c from application threads.
 * Returns ABLY_ERR_CAPACITY if the ring buffer is full. */
ably_error_t rt_enqueue_frame(struct ably_rt_client_s *client,
                               const char *payload, size_t len);

/* Claim the next outbound message serial and increment the counter.
 * Thread-safe: protected by send_mutex.  The caller must NOT hold send_mutex. */
int64_t rt_claim_msg_serial(struct ably_rt_client_s *client);

/* Re-attach all channels that have reattach_pending set.
 * Called by the service thread after CONNECTED is reached. */
void rt_reattach_pending_channels(struct ably_rt_client_s *client);

/* Dispatch a decoded inbound frame to the appropriate handler.
 * Called by the service thread. */
void rt_dispatch_frame(struct ably_rt_client_s *client,
                        const ably_proto_frame_t *frame);

#endif /* ABLY_REALTIME_CLIENT_H */
