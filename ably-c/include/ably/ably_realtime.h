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

    /*
     * Token renewal callback.
     *
     * When a token-authenticated connection is DISCONNECTED with an auth error
     * (Ably error code 401xx), the library calls auth_cb to obtain a fresh token.
     * Write the new token into token_out (NUL-terminated, max token_out_len bytes).
     * Return ABLY_OK on success; any other code keeps the client DISCONNECTED and
     * will be retried on the next reconnect cycle.
     *
     * auth_cb is called from the service thread.  Do not call any ably_rt_* API
     * from inside the callback — use a separate thread or blocking HTTP call.
     */
    ably_error_t (*auth_cb)(ably_rt_client_t *client,
                             char             *token_out,
                             size_t            token_out_len,
                             void             *user_data);
    void *auth_user_data;             /* default: NULL               */

    /*
     * Client identity and authentication.
     *
     * client_id — optional identity string.  Stamped on every published
     *   message and used as the default clientId for presence operations.
     *   Must be set before ably_rt_client_connect().
     *
     * token — pre-supplied Ably capability token.  When set, the WebSocket
     *   URL uses ?access_token=<token> instead of ?key=<api_key>.  Use
     *   ably_rest_request_token() to obtain a token from the REST API.
     *   Must be set before ably_rt_client_connect().
     */
    const char     *client_id;        /* default: NULL (anonymous)   */
    const char     *token;            /* default: NULL (use api_key) */

    /*
     * Path to a PEM-encoded CA certificate bundle to trust instead of the
     * library's built-in CA store.  NULL = use built-in CAs.
     * Useful for private Ably deployments or certificate pinning.
     */
    const char     *ca_cert_pem_path; /* default: NULL               */
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

/*
 * Server-assigned connection ID, populated after reaching CONNECTED state.
 * Returns a pointer to the client's internal buffer — valid for the lifetime
 * of the client.  Empty string ("") before the first CONNECTED is received.
 * Not thread-safe: read only from the CONNECTED state callback or after
 * waiting for CONNECTED state.
 */
const char *ably_rt_client_connection_id(const ably_rt_client_t *client);

/*
 * The clientId set in ably_rt_options_t at create time (or overridden by the
 * server on CONNECTED).  Returns empty string ("") if anonymous.
 * Valid for the lifetime of the client.
 */
const char *ably_rt_client_client_id(const ably_rt_client_t *client);

/*
 * Error information from the most recent server-sent error event.
 * Updated on every DISCONNECTED, ERROR, and FAILED state transition.
 * Returns a pointer to the client's internal buffer — valid for the
 * lifetime of the client.  Thread-safe (read under state_mutex internally).
 */
const ably_error_info_t *ably_rt_client_last_error(const ably_rt_client_t *client);

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
 * Returns ABLY_ERR_STATE if the channel is not ATTACHED or ATTACHING.
 * Returns ABLY_ERR_CAPACITY if the send ring buffer is full.
 */
ably_error_t ably_channel_publish(ably_channel_t *channel,
                                   const char     *name,
                                   const char     *data);

/*
 * Publish a message with an explicit ID for idempotent publishing.
 * id may be NULL (server assigns an ID).  Same state and capacity semantics
 * as ably_channel_publish().
 */
ably_error_t ably_channel_publish_with_id(ably_channel_t *channel,
                                           const char     *name,
                                           const char     *data,
                                           const char     *id);

/* Snapshot of the current channel state (thread-safe). */
ably_channel_state_t ably_channel_state(const ably_channel_t *channel);

/* Channel name — always valid for the lifetime of the client. */
const char *ably_channel_name(const ably_channel_t *channel);

/*
 * Error information from the most recent server-sent channel error.
 * Updated on DETACHED (with error) and channel ERROR frames.
 */
const ably_error_info_t *ably_channel_last_error(const ably_channel_t *channel);

/*
 * Enable VCDIFF delta compression on this channel.
 *
 * When enabled:
 *   - The next ATTACH frame includes params:{"delta":"vcdiff"}, which asks the
 *     Ably server to send subsequent messages as VCDIFF deltas.
 *   - Incoming delta messages are transparently decoded before being passed to
 *     subscribers.  The subscriber callback always receives the full payload.
 *   - The first message after attach is always a full payload (never a delta).
 *
 * Must be called before ably_channel_attach().
 * Returns ABLY_ERR_NOMEM if the internal decode buffers cannot be allocated.
 * Returns ABLY_ERR_STATE if the channel is already ATTACHED or ATTACHING.
 */
ably_error_t ably_channel_enable_delta(ably_channel_t *channel);

/*
 * Request the Ably server to rewind the channel by N messages on ATTACH.
 * The server delivers the last N messages immediately after ATTACHED.
 * Must be called before ably_channel_attach().
 */
void ably_channel_set_rewind(ably_channel_t *channel, int count);

/*
 * Register an occupancy listener for the channel.
 * When set, the ATTACH frame includes params.occupancy="metrics.all", and
 * occupancy updates are delivered via the callback.
 * Must be called before ably_channel_attach().
 */
void ably_channel_set_occupancy_listener(ably_channel_t      *channel,
                                          ably_occupancy_cb_t  cb,
                                          void                *user_data);

/*
 * Set the channel mode bitmask.  Modes control what capabilities are
 * requested from the server (ABLY_CHANNEL_MODE_* flags).
 * 0 = all capabilities (default); otherwise OR together the desired flags.
 * Must be called before ably_channel_attach().
 */
void ably_channel_set_modes(ably_channel_t *channel, uint32_t modes);

/*
 * Return the mode bitmask granted by the server in the most recent
 * ATTACHED frame.  0 if the server did not include a channelMode.
 * Valid only after the channel reaches ATTACHED state.
 */
uint32_t ably_channel_granted_modes(const ably_channel_t *channel);

/*
 * Update the channel options (rewind and/or modes) and trigger a
 * detach + reattach cycle if the channel is currently ATTACHED or ATTACHING.
 * Safe to call from any thread.
 */
ably_error_t ably_channel_set_options(ably_channel_t *channel,
                                       int             rewind,
                                       uint32_t        modes);

/*
 * Fetch historical messages for this channel using the embedded REST client.
 *
 *   limit       — max messages per page; 0 = server default (100)
 *   direction   — "forwards" or "backwards" (NULL = server default)
 *   from_serial — pagination cursor from page->next_cursor; NULL = start
 *   page_out    — receives a heap-allocated page; caller frees with ably_history_page_free()
 *
 * Returns ABLY_OK on HTTP 2xx, ABLY_ERR_HTTP on non-2xx.
 * Returns ABLY_ERR_STATE if the realtime client is not CONNECTED (no REST fallback).
 */
ably_error_t ably_channel_history(ably_channel_t       *channel,
                                   int                   limit,
                                   const char           *direction,
                                   const char           *from_serial,
                                   ably_history_page_t **page_out);

/* ---------------------------------------------------------------------------
 * Presence
 * --------------------------------------------------------------------------- */

/*
 * Enter presence on the channel.
 * client_id identifies this client in the presence set.
 * data is optional presence data (may be NULL).
 * Sends a PRESENCE ENTER frame immediately (channel must be ATTACHED).
 * Returns ABLY_ERR_STATE if the channel is not ATTACHED.
 */
ably_error_t ably_channel_presence_enter(ably_channel_t *channel,
                                          const char     *client_id,
                                          const char     *data);

/*
 * Leave presence on the channel.
 * Returns ABLY_ERR_STATE if not currently entered.
 */
ably_error_t ably_channel_presence_leave(ably_channel_t *channel,
                                          const char     *data);

/*
 * Update presence data without leaving and re-entering.
 * Returns ABLY_ERR_STATE if not currently entered.
 */
ably_error_t ably_channel_presence_update(ably_channel_t *channel,
                                           const char     *data);

/*
 * Subscribe to presence events (ENTER/LEAVE/UPDATE).
 * Returns a positive token on success, 0 if the subscriber limit is reached.
 */
int ably_channel_presence_subscribe(ably_channel_t    *channel,
                                     ably_presence_cb_t cb,
                                     void              *user_data);

/* Unsubscribe by token. */
void ably_channel_presence_unsubscribe(ably_channel_t *channel, int token);

/*
 * Fill out with current PRESENT members.
 * Returns the number of entries written into out[].
 * *count_out is set to the total present member count (may exceed max).
 * out may be NULL to query the total count only.
 */
int ably_channel_presence_get_members(ably_channel_t          *channel,
                                       ably_presence_message_t *out,
                                       int                      max,
                                       int                     *count_out);

/* ---------------------------------------------------------------------------
 * Event loop integration (bring-your-own event loop)
 *
 * These functions provide a non-threaded alternative to ably_rt_client_connect().
 * Use them to integrate ably-c into an existing event loop (libuv, Asio, poll,
 * epoll, etc.) without running a dedicated service thread.
 *
 * Typical usage:
 *   1. ably_rt_client_connect_async()  — TLS + WebSocket handshake (blocking)
 *   2. Register ably_rt_client_fd() with your event loop for readable events
 *   3. On readable: call ably_rt_step(client, 0) to drain inbound frames
 *   4. Call ably_rt_step(client, 0) periodically to flush the outbound ring
 *
 * ably_rt_step() and ably_rt_client_connect_async() are NOT thread-safe with
 * respect to each other — call them from a single event loop thread.
 * --------------------------------------------------------------------------- */

/*
 * Perform the TLS + WebSocket handshake synchronously without starting the
 * background service thread.  The socket is left in non-blocking mode.
 * Returns ABLY_OK on success, ABLY_ERR_NETWORK on connection failure.
 *
 * Do not call ably_rt_client_connect() after this — they are mutually exclusive.
 */
ably_error_t ably_rt_client_connect_async(ably_rt_client_t *client);

/*
 * Execute one iteration of the event loop: poll the socket for up to
 * timeout_ms, drain any inbound frames, flush one outbound frame if queued.
 *
 * Returns  1 if any work was done (frame received or sent).
 * Returns  0 if the poll timed out with no activity.
 * Returns -1 on socket error or disconnection; caller should call
 *          ably_rt_client_connect_async() to reconnect.
 */
int ably_rt_step(ably_rt_client_t *client, int timeout_ms);

/*
 * Return the raw socket file descriptor for the current connection.
 * Returns -1 if not connected.  The fd is invalidated by reconnection.
 * Use with poll()/epoll()/kqueue()/uv_poll_t.
 */
int ably_rt_client_fd(const ably_rt_client_t *client);

/*
 * Return the platform socket handle as a uint64_t.
 * On POSIX: same as ably_rt_client_fd() cast to uint64_t; (uint64_t)-1 if not connected.
 * On Windows: SOCKET cast to uint64_t; (uint64_t)-1 if not connected (INVALID_SOCKET).
 * Use this when you need a portable handle that works on both platforms.
 */
uint64_t ably_rt_client_socket_handle(const ably_rt_client_t *client);

#ifdef __cplusplus
}
#endif
#endif /* ABLY_REALTIME_H */
