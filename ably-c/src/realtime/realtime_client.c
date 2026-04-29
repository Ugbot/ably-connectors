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
 * Real-time client: connection state machine and service thread.
 *
 * The service thread runs a loop:
 *   1. Connect (TCP + TLS + WebSocket handshake)
 *   2. Run event loop: receive frames + drain send ring buffer
 *   3. On disconnect: wait with exponential backoff, then retry
 *
 * Application threads interact via:
 *   - ably_rt_client_connect() — signals the service thread to start
 *   - ably_rt_client_close()   — signals close and waits for CLOSED state
 *   - rt_enqueue_frame()       — pushes frames into the send ring buffer
 *
 * TigerStyle:
 *   - Send ring buffer: fixed capacity, no allocation on enqueue/dequeue
 *   - Protocol frame decode reuses a single frame struct per service thread
 *   - Reconnect backoff uses full-jitter (no allocation)
 */

#include "realtime_client.h"
#include "channel.h"
#include "protocol.h"

#include "ably/ably_realtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Monotonic clock — milliseconds since an arbitrary epoch.
 * Used for heartbeat watchdog only; no wall-clock semantics needed.
 * --------------------------------------------------------------------------- */
static int64_t monotonic_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ---------------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------------- */

void ably_rt_options_init(ably_rt_options_t *opts)
{
    assert(opts != NULL);
    opts->realtime_host               = "realtime.ably.io";
    opts->port                        = 443;
    opts->encoding                    = ABLY_ENCODING_JSON;
    opts->reconnect_initial_delay_ms  = 500;
    opts->reconnect_max_delay_ms      = 60000;
    opts->reconnect_max_attempts      = -1;
    opts->heartbeat_timeout_ms        = 35000;
    opts->tls_verify_peer             = 1;
}

/* ---------------------------------------------------------------------------
 * State transitions
 * --------------------------------------------------------------------------- */

void rt_set_state_locked(ably_rt_client_t *client,
                          ably_connection_state_t new_state,
                          ably_error_t reason)
{
    ably_connection_state_t old_state = client->state;
    if (old_state == new_state) return;

    client->state = new_state;
    ably_cond_broadcast(&client->state_cond);

    /* Invoke callback (outside the lock in the callers — here we call it
     * under the lock for simplicity; the callback must not call back into
     * the client's state-change path). */
    if (client->conn_state_cb) {
        client->conn_state_cb(client, new_state, old_state, reason,
                               client->conn_state_user);
    }
}

/* ---------------------------------------------------------------------------
 * Reconnection backoff: Ably JS SDK formula.
 *
 * delay = base * min((attempt + 2) / 3.0, 2.0) * (1.0 - random * 0.2)
 *
 * This ramps the delay from ~67% of base on attempt 0, up to 200% of base
 * from attempt 3 onward, with ±10% uniform jitter applied on top.
 * Returns delay in ms, clamped to [1, reconnect_max_delay_ms].
 * --------------------------------------------------------------------------- */
static int backoff_delay_ms(const ably_rt_options_t *opts, int attempt)
{
    double base        = (double)opts->reconnect_initial_delay_ms;
    double ramp_factor = (double)(attempt + 2) / 3.0;
    if (ramp_factor > 2.0) ramp_factor = 2.0;

    double jitter = 1.0 - ((double)rand() / (double)RAND_MAX) * 0.2;
    double delay_ms = base * ramp_factor * jitter;

    int delay = (int)delay_ms;
    if (delay < 1)                               delay = 1;
    if (delay > opts->reconnect_max_delay_ms)    delay = opts->reconnect_max_delay_ms;
    return delay;
}

/* ---------------------------------------------------------------------------
 * Outbound ring buffer
 * --------------------------------------------------------------------------- */

ably_error_t rt_enqueue_frame(ably_rt_client_t *client,
                               const char *payload, size_t len)
{
    assert(len < ABLY_FRAME_PAYLOAD_MAX);

    ably_mutex_lock(&client->send_mutex);

    size_t next_head = (client->send_head + 1) % ABLY_SEND_RING_CAPACITY;
    if (next_head == client->send_tail) {
        ably_mutex_unlock(&client->send_mutex);
        return ABLY_ERR_CAPACITY;
    }

    ably_outbound_frame_t *slot = &client->send_ring[client->send_head];
    memcpy(slot->payload, payload, len);
    slot->payload_len = len;
    client->send_head = next_head;

    ably_cond_signal(&client->send_cond);
    ably_mutex_unlock(&client->send_mutex);
    return ABLY_OK;
}

int64_t rt_claim_msg_serial(ably_rt_client_t *client)
{
    ably_mutex_lock(&client->send_mutex);
    int64_t serial = client->outbound_msg_serial++;
    ably_mutex_unlock(&client->send_mutex);
    return serial;
}

static ably_outbound_frame_t *ring_peek(ably_rt_client_t *client)
{
    if (client->send_head == client->send_tail) return NULL;
    return &client->send_ring[client->send_tail];
}

static void ring_consume(ably_rt_client_t *client)
{
    client->send_tail = (client->send_tail + 1) % ABLY_SEND_RING_CAPACITY;
}

/* ---------------------------------------------------------------------------
 * Frame dispatch (service thread)
 * --------------------------------------------------------------------------- */

void rt_dispatch_frame(ably_rt_client_t *client, const ably_proto_frame_t *frame)
{
    switch (frame->action) {

    case ABLY_ACTION_HEARTBEAT:
        /* Echo heartbeat back to server and reset watchdog. */
        client->last_activity_ms = monotonic_ms();
        {
            char hb[32];
            size_t n = ably_proto_encode_heartbeat(hb, sizeof(hb),
                                                    client->opts.encoding);
            if (n > 0) ably_ws_send_text(client->ws, hb, n);
        }
        break;

    case ABLY_ACTION_CONNECTED:
        ABLY_LOG_I(&client->log, "Ably CONNECTED");
        client->last_activity_ms = monotonic_ms();
        ably_mutex_lock(&client->state_mutex);
        rt_set_state_locked(client, ABLY_CONN_CONNECTED, ABLY_OK);
        ably_mutex_unlock(&client->state_mutex);
        client->reconnect_attempt = 0;
        rt_reattach_pending_channels(client);
        break;

    case ABLY_ACTION_DISCONNECTED:
        ABLY_LOG_I(&client->log, "Ably DISCONNECTED (code=%d msg=%s)",
                   frame->error_code,
                   frame->error_message ? frame->error_message : "");
        /* Service loop will notice disconnection and reconnect. */
        break;

    case ABLY_ACTION_ERROR:
        ABLY_LOG_E(&client->log, "Ably ERROR code=%d: %s",
                   frame->error_code,
                   frame->error_message ? frame->error_message : "");
        /* 4xxxx codes are fatal (auth failures, bad key, etc.) */
        if (frame->error_code >= 40000 && frame->error_code < 50000) {
            ably_mutex_lock(&client->state_mutex);
            rt_set_state_locked(client, ABLY_CONN_FAILED, ABLY_ERR_AUTH);
            client->close_requested = 1;
            ably_cond_broadcast(&client->state_cond);
            ably_mutex_unlock(&client->state_mutex);
        }
        break;

    case ABLY_ACTION_CLOSED:
        ABLY_LOG_I(&client->log, "Ably CLOSED");
        ably_mutex_lock(&client->state_mutex);
        rt_set_state_locked(client, ABLY_CONN_CLOSED, ABLY_OK);
        ably_cond_broadcast(&client->state_cond);
        ably_mutex_unlock(&client->state_mutex);
        break;

    case ABLY_ACTION_ATTACHED:
    case ABLY_ACTION_DETACHED:
    case ABLY_ACTION_MESSAGE:
        /* Delegate to channel layer. */
        if (frame->channel) {
            ably_mutex_lock(&client->chan_mutex);
            for (size_t i = 0; i < client->channel_count; i++) {
                if (strcmp(client->channels[i]->name, frame->channel) == 0) {
                    ably_channel_t *ch = client->channels[i];
                    ably_mutex_unlock(&client->chan_mutex);
                    ably_channel_on_frame(ch, frame);
                    goto done;
                }
            }
            ably_mutex_unlock(&client->chan_mutex);
        }
        break;

    default:
        ABLY_LOG_D(&client->log, "Unhandled action=%d", (int)frame->action);
        break;
    }
done:;
}

/* ---------------------------------------------------------------------------
 * Inbound frame callback (called by ws_client on each received text frame)
 * --------------------------------------------------------------------------- */

static void on_ws_frame(const char *buf, size_t len, void *user_data)
{
    ably_rt_client_t *client = user_data;

    /* Re-use the decode_frame struct (service thread only). */
    ably_proto_frame_t *frame = &client->decode_frame;
    frame->messages    = client->decode_msgs;
    frame->message_cap = 32;
    frame->message_count = 0;

    ably_error_t err;
    if (client->opts.encoding == ABLY_ENCODING_MSGPACK) {
        err = ably_proto_decode_msgpack((const uint8_t *)buf, len, frame);
    } else {
        err = ably_proto_decode_json(buf, len, frame);
    }
    if (err != ABLY_OK) {
        ABLY_LOG_W(&client->log, "Failed to decode inbound frame (len=%zu)", len);
        return;
    }

    rt_dispatch_frame(client, frame);
}

/* ---------------------------------------------------------------------------
 * Service thread
 * --------------------------------------------------------------------------- */

static ABLY_THREAD_FUNC service_thread_fn(void *arg)
{
    ably_rt_client_t *client = arg;
    srand((unsigned)time(NULL));

    while (1) {
        /* Check close_requested before attempting to connect. */
        ably_mutex_lock(&client->state_mutex);
        if (client->close_requested) {
            rt_set_state_locked(client, ABLY_CONN_CLOSED, ABLY_OK);
            ably_mutex_unlock(&client->state_mutex);
            break;
        }
        rt_set_state_locked(client, ABLY_CONN_CONNECTING, ABLY_OK);
        ably_mutex_unlock(&client->state_mutex);

        ABLY_LOG_I(&client->log, "Connecting to %s (attempt %d)",
                   client->opts.realtime_host, client->reconnect_attempt);

        client->last_activity_ms = 0;  /* reset watchdog; set again on CONNECTED */

        /* msgSerial resets to 0 on every new connection per Ably protocol spec. */
        ably_mutex_lock(&client->send_mutex);
        client->outbound_msg_serial = 0;
        ably_mutex_unlock(&client->send_mutex);

        ably_error_t err = ably_ws_connect(client->ws);
        if (err != ABLY_OK) {
            ABLY_LOG_W(&client->log, "Connect failed");
            goto disconnected;
        }

        /* Event loop: receive frames and drain send ring. */
        while (ably_ws_is_connected(client->ws)) {
            ably_mutex_lock(&client->state_mutex);
            int should_close = client->close_requested;
            ably_mutex_unlock(&client->state_mutex);

            if (should_close) {
                /* Send Ably CLOSE message then do the WebSocket-level close. */
                char close_buf[32];
                size_t close_len = ably_proto_encode_close(close_buf,
                                                            sizeof(close_buf),
                                                            client->opts.encoding);
                if (close_len > 0) ably_ws_send_text(client->ws, close_buf, close_len);

                ably_ws_close(client->ws, 5000);

                /* Transition to CLOSED and wake ably_rt_client_close(). The server
                 * may have already dispatched ABLY_ACTION_CLOSED (which also sets
                 * this state), but if it didn't we must set it here so the waiting
                 * thread is not left stuck until timeout. */
                ably_mutex_lock(&client->state_mutex);
                rt_set_state_locked(client, ABLY_CONN_CLOSED, ABLY_OK);
                ably_cond_broadcast(&client->state_cond);
                ably_mutex_unlock(&client->state_mutex);
                break;
            }

            /* Drain outbound ring buffer. */
            ably_mutex_lock(&client->send_mutex);
            ably_outbound_frame_t *frame = ring_peek(client);
            if (frame) {
                char payload_copy[ABLY_FRAME_PAYLOAD_MAX];
                size_t plen = frame->payload_len;
                memcpy(payload_copy, frame->payload, plen);
                ring_consume(client);
                ably_mutex_unlock(&client->send_mutex);

                ably_ws_send_text(client->ws, payload_copy, plen);
            } else {
                ably_mutex_unlock(&client->send_mutex);
            }

            /* Receive (non-blocking poll, 100 ms timeout). */
            ably_ws_recv_once(client->ws, 100);

            /* Heartbeat watchdog: if no activity for heartbeat_timeout_ms, treat
             * as a stale connection and force a reconnect. */
            if (client->last_activity_ms != 0 &&
                client->opts.heartbeat_timeout_ms > 0) {
                int64_t elapsed = monotonic_ms() - client->last_activity_ms;
                if (elapsed > client->opts.heartbeat_timeout_ms) {
                    ABLY_LOG_W(&client->log,
                               "Heartbeat timeout after %ldms — reconnecting",
                               (long)elapsed);
                    break;
                }
            }
        }

        /* Check if we should stop or reconnect. */
        ably_mutex_lock(&client->state_mutex);
        int should_stop = client->close_requested
                       || client->state == ABLY_CONN_FAILED
                       || client->state == ABLY_CONN_CLOSED;
        ably_mutex_unlock(&client->state_mutex);

        if (should_stop) break;

disconnected:
        ably_mutex_lock(&client->state_mutex);
        rt_set_state_locked(client, ABLY_CONN_DISCONNECTED, ABLY_ERR_NETWORK);
        ably_mutex_unlock(&client->state_mutex);

        /* Mark all attached channels as pending reattach. */
        ably_mutex_lock(&client->chan_mutex);
        for (size_t i = 0; i < client->channel_count; i++) {
            ably_channel_on_disconnect(client->channels[i]);
        }
        ably_mutex_unlock(&client->chan_mutex);

        /* Check max attempts. */
        int max = client->opts.reconnect_max_attempts;
        if (max > 0 && client->reconnect_attempt >= max) {
            ABLY_LOG_W(&client->log, "Max reconnect attempts reached — SUSPENDED");
            ably_mutex_lock(&client->state_mutex);
            rt_set_state_locked(client, ABLY_CONN_SUSPENDED, ABLY_ERR_NETWORK);
            /* Wait 60s in SUSPENDED, then retry. */
            ably_cond_timedwait_ms(&client->state_cond, &client->state_mutex, 60000);
            ably_mutex_unlock(&client->state_mutex);
            client->reconnect_attempt = 0;
        }

        /* Backoff delay. */
        int delay = backoff_delay_ms(&client->opts, client->reconnect_attempt);
        client->reconnect_attempt++;

        ABLY_LOG_I(&client->log, "Reconnecting in %d ms (attempt %d)",
                   delay, client->reconnect_attempt);

        /* Interruptible sleep via cond_timedwait. */
        ably_mutex_lock(&client->state_mutex);
        if (!client->close_requested) {
            ably_cond_timedwait_ms(&client->state_cond, &client->state_mutex, delay);
        }
        ably_mutex_unlock(&client->state_mutex);
    }

    ABLY_LOG_I(&client->log, "Service thread exiting");
    return (ABLY_THREAD_FUNC)(intptr_t)0;
}

/* ---------------------------------------------------------------------------
 * Re-attach pending channels after reconnect
 * --------------------------------------------------------------------------- */

void rt_reattach_pending_channels(ably_rt_client_t *client)
{
    ably_mutex_lock(&client->chan_mutex);
    for (size_t i = 0; i < client->channel_count; i++) {
        ably_channel_t *ch = client->channels[i];
        if (ably_channel_needs_reattach(ch)) {
            char buf[ABLY_MAX_CHANNEL_NAME_LEN + 32];
            size_t n = ably_proto_encode_attach(buf, sizeof(buf), ch->name,
                                                client->opts.encoding);
            if (n > 0) {
                rt_enqueue_frame(client, buf, n);
                ably_channel_set_attaching(ch);
            }
        }
    }
    ably_mutex_unlock(&client->chan_mutex);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

ably_rt_client_t *ably_rt_client_create(const char              *api_key,
                                          const ably_rt_options_t *opts,
                                          const ably_allocator_t  *allocator)
{
    if (!api_key || *api_key == '\0') return NULL;

    ably_rt_options_t defaults;
    if (!opts) { ably_rt_options_init(&defaults); opts = &defaults; }

    ably_allocator_t a = allocator ? *allocator : ably_system_allocator();

    ably_rt_client_t *c = ably_mem_malloc(&a, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));

    c->alloc = a;
    c->opts  = *opts;
    snprintf(c->api_key, sizeof(c->api_key), "%s", api_key);

    /* Build WebSocket URL path. */
    snprintf(c->ws_path, sizeof(c->ws_path),
             "/?v=3&key=%s&format=%s",
             api_key,
             opts->encoding == ABLY_ENCODING_MSGPACK ? "msgpack" : "json");

    /* Pre-allocate send ring buffer. */
    c->send_ring = ably_mem_malloc(&a,
        ABLY_SEND_RING_CAPACITY * sizeof(ably_outbound_frame_t));
    if (!c->send_ring) goto fail;

    ably_mutex_init(&c->state_mutex);
    ably_cond_init(&c->state_cond);
    ably_mutex_init(&c->send_mutex);
    ably_cond_init(&c->send_cond);
    ably_mutex_init(&c->chan_mutex);

    c->state = ABLY_CONN_INITIALIZED;

    /* Create WebSocket client. */
    ably_ws_options_t wo;
    wo.host            = opts->realtime_host;
    wo.port            = opts->port;
    wo.path            = c->ws_path;
    wo.timeout_ms      = 10000;
    wo.tls_verify_peer = opts->tls_verify_peer;

    c->ws = ably_ws_client_create(&wo, on_ws_frame, c, &a, &c->log);
    if (!c->ws) goto fail;

    c->decode_frame.messages    = c->decode_msgs;
    c->decode_frame.message_cap = 32;

    return c;

fail:
    if (c->send_ring) ably_mem_free(&a, c->send_ring);
    if (c->ws)        ably_ws_client_destroy(c->ws);
    ably_mem_free(&a, c);
    return NULL;
}

void ably_rt_client_set_conn_state_cb(ably_rt_client_t  *client,
                                       ably_conn_state_cb cb,
                                       void              *user_data)
{
    assert(client != NULL);
    client->conn_state_cb   = cb;
    client->conn_state_user = user_data;
}

void ably_rt_client_set_log_cb(ably_rt_client_t *client,
                                ably_log_cb cb, void *user_data)
{
    assert(client != NULL);
    client->log.cb        = cb;
    client->log.user_data = user_data;
}

ably_error_t ably_rt_client_connect(ably_rt_client_t *client)
{
    assert(client != NULL);

    client->close_requested = 0;
    int ret = ably_thread_create(&client->service_thread,
                                  service_thread_fn, client);
    if (ret != 0) return ABLY_ERR_THREAD;

    client->service_thread_running = 1;
    return ABLY_OK;
}

ably_error_t ably_rt_client_close(ably_rt_client_t *client, int timeout_ms)
{
    assert(client != NULL);
    if (timeout_ms <= 0) timeout_ms = 5000;

    ably_mutex_lock(&client->state_mutex);
    client->close_requested = 1;
    ably_cond_broadcast(&client->state_cond);  /* wake backoff sleep */
    ably_mutex_unlock(&client->state_mutex);

    /* Wait for CLOSED or FAILED state. */
    ably_mutex_lock(&client->state_mutex);
    int timed_out = 0;
    while (client->state != ABLY_CONN_CLOSED &&
           client->state != ABLY_CONN_FAILED  &&
           !timed_out) {
        timed_out = ably_cond_timedwait_ms(&client->state_cond,
                                            &client->state_mutex, timeout_ms);
    }
    ably_mutex_unlock(&client->state_mutex);

    if (client->service_thread_running) {
        ably_thread_join(client->service_thread);
        client->service_thread_running = 0;
    }

    return timed_out ? ABLY_ERR_TIMEOUT : ABLY_OK;
}

void ably_rt_client_destroy(ably_rt_client_t *client)
{
    if (!client) return;

    if (client->service_thread_running) {
        ably_rt_client_close(client, 5000);
    }

    ably_ws_client_destroy(client->ws);
    ably_mem_free(&client->alloc, client->send_ring);

    ably_mutex_destroy(&client->state_mutex);
    ably_cond_destroy(&client->state_cond);
    ably_mutex_destroy(&client->send_mutex);
    ably_cond_destroy(&client->send_cond);
    ably_mutex_destroy(&client->chan_mutex);

    /* Channels are owned by the client; free them. */
    for (size_t i = 0; i < client->channel_count; i++) {
        ably_channel_destroy(client->channels[i]);
    }

    ably_mem_free(&client->alloc, client);
}

ably_connection_state_t ably_rt_client_state(const ably_rt_client_t *client)
{
    assert(client != NULL);
    /* Snapshot under lock for memory visibility on weakly-ordered platforms. */
    ably_mutex_lock((ably_mutex_t *)&client->state_mutex);
    ably_connection_state_t s = client->state;
    ably_mutex_unlock((ably_mutex_t *)&client->state_mutex);
    return s;
}

ably_channel_t *ably_rt_channel_get(ably_rt_client_t *client, const char *name)
{
    assert(client != NULL);
    assert(name != NULL);

    if (strlen(name) >= ABLY_MAX_CHANNEL_NAME_LEN) return NULL;

    ably_mutex_lock(&client->chan_mutex);

    /* Look for existing channel. */
    for (size_t i = 0; i < client->channel_count; i++) {
        if (strcmp(client->channels[i]->name, name) == 0) {
            ably_channel_t *ch = client->channels[i];
            ably_mutex_unlock(&client->chan_mutex);
            return ch;
        }
    }

    if (client->channel_count >= ABLY_MAX_CHANNELS) {
        ably_mutex_unlock(&client->chan_mutex);
        return NULL;
    }

    /* Create new channel. */
    ably_channel_t *ch = ably_channel_create(client, name, &client->alloc,
                                              &client->log);
    if (!ch) {
        ably_mutex_unlock(&client->chan_mutex);
        return NULL;
    }

    client->channels[client->channel_count++] = ch;
    ably_mutex_unlock(&client->chan_mutex);
    return ch;
}
