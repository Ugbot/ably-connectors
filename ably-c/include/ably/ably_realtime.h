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

#ifndef ABLY_REALTIME_H
#define ABLY_REALTIME_H

#include "ably_types.h"
#include "ably_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Real-time client options
 * --------------------------------------------------------------------------- */
typedef struct {
    const char     *realtime_host;             /* default: "realtime.ably.io" */
    uint16_t        port;                      /* default: 443                */
    ably_encoding_t encoding;                  /* default: ABLY_ENCODING_JSON */

    /* Reconnection: full-jitter exponential backoff */
    int reconnect_initial_delay_ms;   /* default: 500                */
    int reconnect_max_delay_ms;       /* default: 60000              */
    int reconnect_max_attempts;       /* default: -1 (unlimited)     */

    /* How long to wait without a heartbeat before treating the connection
     * as lost.  Ably sends action=0 (HEARTBEAT) periodically. */
    int heartbeat_timeout_ms;         /* default: 35000              */

    int tls_verify_peer;              /* default: 1                  */
} ably_rt_options_t;

void ably_rt_options_init(ably_rt_options_t *opts);

/* ---------------------------------------------------------------------------
 * Real-time client lifecycle
 * --------------------------------------------------------------------------- */

/*
 * Create a real-time client.  No threads are started yet.
 * api_key must be "keyId:keySecret".
 * opts may be NULL; allocator may be NULL.
 */
ably_rt_client_t *ably_rt_client_create(const char              *api_key,
                                          const ably_rt_options_t *opts,
                                          const ably_allocator_t  *allocator);

/* Register a connection-state change callback.
 * Must be called before ably_rt_client_connect(). */
void ably_rt_client_set_conn_state_cb(ably_rt_client_t   *client,
                                       ably_conn_state_cb  cb,
                                       void               *user_data);

/* Override the log callback. */
void ably_rt_client_set_log_cb(ably_rt_client_t *client,
                                ably_log_cb       cb,
                                void             *user_data);

/*
 * Initiate the WebSocket connection.  Spawns the background service thread.
 * Non-blocking: returns immediately.  Monitor state via the conn_state_cb.
 * Returns ABLY_ERR_THREAD if thread creation fails.
 */
ably_error_t ably_rt_client_connect(ably_rt_client_t *client);

/*
 * Gracefully close the connection.  Sends WebSocket CLOSE frame and waits
 * up to timeout_ms for the CLOSED state.  Blocks the calling thread.
 * Pass timeout_ms <= 0 to use the default (5000 ms).
 */
ably_error_t ably_rt_client_close(ably_rt_client_t *client, int timeout_ms);

/* Destroy the client.  Closes first if still connected.  Safe with NULL. */
void ably_rt_client_destroy(ably_rt_client_t *client);

/* Snapshot of the current connection state (thread-safe). */
ably_connection_state_t ably_rt_client_state(const ably_rt_client_t *client);

/* ---------------------------------------------------------------------------
 * Channels
 * --------------------------------------------------------------------------- */

/*
 * Get or create a channel by name.  Idempotent: the same name always returns
 * the same handle.  The handle is owned by the client; do not free it.
 *
 * Returns NULL if the channel name exceeds ABLY_MAX_CHANNEL_NAME_LEN or if
 * the client has reached ABLY_MAX_CHANNELS capacity.
 */
ably_channel_t *ably_rt_channel_get(ably_rt_client_t *client,
                                     const char       *name);

/* Register a channel state-change callback. */
void ably_channel_set_state_cb(ably_channel_t    *channel,
                                ably_chan_state_cb  cb,
                                void              *user_data);

/*
 * Attach the channel: send ATTACH to Ably, transition to ATTACHING.
 * The channel moves to ATTACHED when the server confirms with ATTACHED.
 * Non-blocking.  Returns ABLY_ERR_STATE if the client is not CONNECTED.
 */
ably_error_t ably_channel_attach(ably_channel_t *channel);

/*
 * Detach the channel.  Non-blocking.
 * Returns ABLY_ERR_STATE if the channel is not ATTACHED or ATTACHING.
 */
ably_error_t ably_channel_detach(ably_channel_t *channel);

/*
 * Subscribe to messages on the channel.
 *   name_filter — if non-NULL, only messages whose `name` field matches
 *                 are delivered to this subscriber.  NULL = all messages.
 * Thread-safe.
 * Returns a positive subscription token on success, negative on error.
 * Returns ABLY_ERR_CAPACITY if ABLY_MAX_SUBSCRIBERS_PER_CHANNEL is reached.
 */
int ably_channel_subscribe(ably_channel_t  *channel,
                            const char      *name_filter,
                            ably_message_cb  cb,
                            void            *user_data);

/* Unsubscribe using the token returned by ably_channel_subscribe(). */
ably_error_t ably_channel_unsubscribe(ably_channel_t *channel, int token);

/*
 * Publish a message via the real-time connection.
 * Enqueues the message into the outbound ring buffer; actual transmission
 * happens on the service thread.  Thread-safe.
 * Returns ABLY_ERR_STATE if the channel is not ATTACHED.
 * Returns ABLY_ERR_CAPACITY if the send ring buffer is full.
 */
ably_error_t ably_channel_publish(ably_channel_t *channel,
                                   const char     *name,
                                   const char     *data);

/* Snapshot of the current channel state (thread-safe). */
ably_channel_state_t ably_channel_state(const ably_channel_t *channel);

/* Channel name — always valid for the lifetime of the client. */
const char *ably_channel_name(const ably_channel_t *channel);

#ifdef __cplusplus
}
#endif
#endif /* ABLY_REALTIME_H */
