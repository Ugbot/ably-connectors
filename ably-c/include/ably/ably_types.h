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

#ifndef ABLY_TYPES_H
#define ABLY_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Compile-time capacity constants.
 *
 * These govern the fixed-size pre-allocated structures used throughout the
 * library.  No hot-path code allocates; all memory is claimed during client
 * creation.  Override via -DABLY_MAX_CHANNELS=N etc. at compile time.
 * --------------------------------------------------------------------------- */

#ifndef ABLY_MAX_CHANNELS
#  define ABLY_MAX_CHANNELS            64
#endif

#ifndef ABLY_MAX_CHANNEL_NAME_LEN
#  define ABLY_MAX_CHANNEL_NAME_LEN    256
#endif

#ifndef ABLY_MAX_SUBSCRIBERS_PER_CHANNEL
#  define ABLY_MAX_SUBSCRIBERS_PER_CHANNEL  32
#endif

/* Ring buffer capacity for outbound frames (must be power of two). */
#ifndef ABLY_SEND_RING_CAPACITY
#  define ABLY_SEND_RING_CAPACITY      256
#endif

/* Maximum size of a single outbound frame payload (bytes). */
#ifndef ABLY_MAX_FRAME_SIZE
#  define ABLY_MAX_FRAME_SIZE          (64 * 1024)
#endif

/* Receive buffer size (bytes); must fit the largest expected inbound frame. */
#ifndef ABLY_RECV_BUF_SIZE
#  define ABLY_RECV_BUF_SIZE           (128 * 1024)
#endif

/* Maximum length of API key (keyId + ':' + keySecret). */
#ifndef ABLY_MAX_KEY_LEN
#  define ABLY_MAX_KEY_LEN             256
#endif

/* Maximum length of a message name or data field. */
#ifndef ABLY_MAX_MESSAGE_NAME_LEN
#  define ABLY_MAX_MESSAGE_NAME_LEN    256
#endif

#ifndef ABLY_MAX_MESSAGE_DATA_LEN
#  define ABLY_MAX_MESSAGE_DATA_LEN    (32 * 1024)
#endif

/* Maximum length of a client ID string. */
#ifndef ABLY_MAX_CLIENT_ID_LEN
#  define ABLY_MAX_CLIENT_ID_LEN       256
#endif

/* Maximum presence members tracked per channel. Override with -DABLY_MAX_PRESENCE_MEMBERS=N. */
#ifndef ABLY_MAX_PRESENCE_MEMBERS
#  define ABLY_MAX_PRESENCE_MEMBERS    128
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * --------------------------------------------------------------------------- */
typedef enum {
    ABLY_OK               =  0,
    ABLY_ERR_NOMEM        = -1,
    ABLY_ERR_INVALID_ARG  = -2,
    ABLY_ERR_AUTH         = -3,
    ABLY_ERR_NETWORK      = -4,
    ABLY_ERR_HTTP         = -5,
    ABLY_ERR_PROTOCOL     = -6,
    ABLY_ERR_TIMEOUT      = -7,
    ABLY_ERR_STATE        = -8,
    ABLY_ERR_THREAD       = -9,
    ABLY_ERR_CAPACITY     = -10,  /* fixed-size structure is full */
    ABLY_ERR_INTERNAL     = -99,
} ably_error_t;

const char *ably_error_str(ably_error_t err);

/* ---------------------------------------------------------------------------
 * Wire encoding
 * --------------------------------------------------------------------------- */
typedef enum {
    ABLY_ENCODING_JSON    = 0,
    ABLY_ENCODING_MSGPACK = 1,
} ably_encoding_t;

/* ---------------------------------------------------------------------------
 * Connection states (mirror the Ably spec)
 * --------------------------------------------------------------------------- */
typedef enum {
    ABLY_CONN_INITIALIZED  = 0,
    ABLY_CONN_CONNECTING   = 1,
    ABLY_CONN_CONNECTED    = 2,
    ABLY_CONN_DISCONNECTED = 3,
    ABLY_CONN_SUSPENDED    = 4,
    ABLY_CONN_CLOSING      = 5,
    ABLY_CONN_CLOSED       = 6,
    ABLY_CONN_FAILED       = 7,
} ably_connection_state_t;

/* ---------------------------------------------------------------------------
 * Channel states
 * --------------------------------------------------------------------------- */
typedef enum {
    ABLY_CHAN_INITIALIZED  = 0,
    ABLY_CHAN_ATTACHING    = 1,
    ABLY_CHAN_ATTACHED     = 2,
    ABLY_CHAN_DETACHING    = 3,
    ABLY_CHAN_DETACHED     = 4,
    ABLY_CHAN_FAILED       = 5,
} ably_channel_state_t;

/* ---------------------------------------------------------------------------
 * Inbound message.
 *
 * All pointer fields point into the library's internal receive buffer and are
 * valid ONLY for the duration of the ably_message_cb invocation.  If you need
 * the data beyond the callback, copy it.
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *id;          /* Ably-assigned message ID (may be NULL) */
    const char *name;        /* event name (may be NULL)               */
    const char *data;        /* payload (UTF-8 string)                  */
    const char *client_id;   /* publisher client ID (may be NULL)      */
    int64_t     timestamp;   /* milliseconds since Unix epoch           */
} ably_message_t;

/* ---------------------------------------------------------------------------
 * Opaque handles — internals are not public
 * --------------------------------------------------------------------------- */
typedef struct ably_rest_client_s  ably_rest_client_t;
typedef struct ably_rt_client_s    ably_rt_client_t;
typedef struct ably_channel_s      ably_channel_t;

/* ---------------------------------------------------------------------------
 * Presence
 * --------------------------------------------------------------------------- */

/* Presence action codes — mirrors the Ably JS SDK PresenceMessage.Action enum. */
typedef enum {
    ABLY_PRESENCE_ABSENT  = 0,
    ABLY_PRESENCE_PRESENT = 1,
    ABLY_PRESENCE_ENTER   = 2,
    ABLY_PRESENCE_LEAVE   = 3,
    ABLY_PRESENCE_UPDATE  = 4,
} ably_presence_action_t;

/*
 * A single presence message.
 *
 * Fields are value-copied into fixed-size buffers; they are valid for the
 * entire duration of the presence callback (and may be retained by the
 * caller indefinitely since they are not tied to the receive buffer).
 */
typedef struct {
    ably_presence_action_t action;
    char                   client_id[ABLY_MAX_CLIENT_ID_LEN];
    char                   connection_id[64];
    char                   data[ABLY_MAX_MESSAGE_DATA_LEN];
    int64_t                timestamp;
} ably_presence_message_t;

/* ---------------------------------------------------------------------------
 * Occupancy metrics
 * --------------------------------------------------------------------------- */

typedef struct {
    int connections;
    int publishers;
    int subscribers;
    int presence_connections;
    int presence_members;
    int presence_subscribers;
} ably_occupancy_t;

/* ---------------------------------------------------------------------------
 * History
 *
 * ably_history_page_t is heap-allocated by ably_rest_channel_history().
 * The caller must free it with ably_history_page_free().
 * --------------------------------------------------------------------------- */

typedef struct {
    ably_message_t *items;
    size_t          count;
    char            next_cursor[256];  /* empty string = last page */
} ably_history_page_t;

/* ---------------------------------------------------------------------------
 * Channel status (REST)
 * --------------------------------------------------------------------------- */

typedef struct {
    char             name[ABLY_MAX_CHANNEL_NAME_LEN];
    ably_occupancy_t occupancy;
    int              is_active;
} ably_channel_status_t;

/* ---------------------------------------------------------------------------
 * Error information (from server-side error objects)
 *
 * Returned by ably_rt_client_last_error() and ably_channel_last_error().
 * Provides the Ably numeric error code and server message from the most
 * recent error event on the client or channel.
 * --------------------------------------------------------------------------- */
typedef struct {
    int  ably_code;     /* Ably error code (e.g. 40142, 80002); 0 = none */
    char message[256];  /* server error message string; "" = none         */
} ably_error_info_t;

/* ---------------------------------------------------------------------------
 * Channel mode flags
 *
 * Passed to ably_channel_set_modes().  Send as a bitmask in ATTACH frames.
 * The server may grant fewer modes than requested; check the granted set
 * with ably_channel_granted_modes() after ATTACHED.
 * --------------------------------------------------------------------------- */
#define ABLY_CHANNEL_MODE_PRESENCE           0x01u
#define ABLY_CHANNEL_MODE_PUBLISH            0x02u
#define ABLY_CHANNEL_MODE_SUBSCRIBE          0x04u
#define ABLY_CHANNEL_MODE_PRESENCE_SUBSCRIBE 0x08u
#define ABLY_CHANNEL_MODE_PUSH_SUBSCRIBE     0x10u
#define ABLY_CHANNEL_MODE_PUSH_ADMIN         0x20u

/* ---------------------------------------------------------------------------
 * Stats (REST GET /stats)
 * --------------------------------------------------------------------------- */

/* Leaf count+data pair used throughout the stats response. */
typedef struct {
    double count;
    double data;    /* bytes transferred */
} ably_stats_count_t;

/* Breakdown by message type (all messages, channel messages, presence messages). */
typedef struct {
    ably_stats_count_t all;
    ably_stats_count_t messages;
    ably_stats_count_t presence;
} ably_stats_message_types_t;

/* Breakdown by transport/endpoint. */
typedef struct {
    ably_stats_message_types_t all;
    ably_stats_message_types_t realtime;
    ably_stats_message_types_t rest;
    ably_stats_message_types_t webhook;
    ably_stats_message_types_t push;
    ably_stats_message_types_t http_event;
    ably_stats_message_types_t shared_queue;
} ably_stats_message_traffic_t;

typedef struct {
    char   interval_id[32];   /* e.g. "2024-01-01:00:00", "2024-01-01:00", "2024-01-01" */
    char   unit[16];          /* "minute" | "hour" | "day" | "month" */

    ably_stats_message_traffic_t inbound;
    ably_stats_message_traffic_t outbound;
    ably_stats_message_types_t   persisted;

    struct { double peak; double min; double opened; double refused; double closed; } connections;
    struct { double peak; double min; double opened; double refused; double closed; } channels;

    struct { double succeeded; double failed; double refused; } api_requests;
    struct { double succeeded; double failed; double refused; } token_requests;
} ably_stats_t;

/* Heap-allocated page returned by ably_rest_stats(). Free with ably_stats_page_free(). */
typedef struct {
    ably_stats_t *items;
    size_t        count;
    char          next_cursor[256];   /* empty = last page */
} ably_stats_page_t;

/* ---------------------------------------------------------------------------
 * Callbacks
 * --------------------------------------------------------------------------- */

/* Invoked from the service thread when a message arrives on a channel. */
typedef void (*ably_message_cb)(ably_channel_t       *channel,
                                const ably_message_t *message,
                                void                 *user_data);

/* Invoked on presence enter/leave/update/present events. */
typedef void (*ably_presence_cb_t)(ably_channel_t              *channel,
                                   const ably_presence_message_t *msg,
                                   void                          *user_data);

/* Invoked when realtime occupancy metrics change (requires occupancy listener). */
typedef void (*ably_occupancy_cb_t)(ably_channel_t       *channel,
                                    const ably_occupancy_t *occ,
                                    void                   *user_data);

/* Invoked from the service thread on connection state transitions. */
typedef void (*ably_conn_state_cb)(ably_rt_client_t        *client,
                                   ably_connection_state_t  new_state,
                                   ably_connection_state_t  old_state,
                                   ably_error_t             reason,
                                   void                    *user_data);

/* Invoked from the service thread on channel state transitions. */
typedef void (*ably_chan_state_cb)(ably_channel_t      *channel,
                                   ably_channel_state_t new_state,
                                   ably_channel_state_t old_state,
                                   ably_error_t         reason,
                                   void                *user_data);

/*
 * Log callback.  level: 0=ERROR 1=WARN 2=INFO 3=DEBUG.
 * Install via ably_rt_client_set_log_cb() or ably_rest_client_set_log_cb().
 * Default: writes to stderr.
 */
typedef void (*ably_log_cb)(int         level,
                             const char *file,
                             int         line,
                             const char *message,
                             void       *user_data);

#ifdef __cplusplus
}
#endif
#endif /* ABLY_TYPES_H */
