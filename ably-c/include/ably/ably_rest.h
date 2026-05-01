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

#ifndef ABLY_REST_H
#define ABLY_REST_H

#include "ably_types.h"
#include "ably_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * REST client options
 * --------------------------------------------------------------------------- */
typedef struct {
    const char     *rest_host;        /* default: "rest.ably.io"  */
    uint16_t        port;             /* default: 443              */
    long            timeout_ms;       /* default: 10000            */
    int             tls_verify_peer;  /* default: 1                */
    ably_encoding_t encoding;         /* default: ABLY_ENCODING_JSON */
    const char     *ca_cert_pem_path; /* default: NULL (built-in)  */
} ably_rest_options_t;

/* Fill opts with library defaults.  Always call before customising. */
void ably_rest_options_init(ably_rest_options_t *opts);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

/*
 * Create a REST client.  api_key must be "keyId:keySecret".
 * opts may be NULL (use defaults).  allocator may be NULL (system malloc).
 * Returns NULL on allocation failure or invalid api_key.
 */
ably_rest_client_t *ably_rest_client_create(const char                *api_key,
                                             const ably_rest_options_t *opts,
                                             const ably_allocator_t    *allocator);

/* Override the log callback.  Must be called before first publish. */
void ably_rest_client_set_log_cb(ably_rest_client_t *client,
                                  ably_log_cb         cb,
                                  void               *user_data);

/* Destroy a REST client and free all resources.  Safe to call with NULL. */
void ably_rest_client_destroy(ably_rest_client_t *client);

/* ---------------------------------------------------------------------------
 * Publishing
 *
 * All publish functions block until the HTTP response is received or the
 * timeout elapses.  They are safe to call from any thread as long as a
 * single client is not shared across threads concurrently.
 * --------------------------------------------------------------------------- */

/*
 * Publish a single message.
 *   channel  — Ably channel name (percent-encoded by the library).
 *   name     — event name; may be NULL.
 *   data     — UTF-8 payload; may be NULL.
 * Returns ABLY_OK on HTTP 2xx.
 * Returns ABLY_ERR_HTTP on non-2xx; inspect ably_rest_last_http_status().
 */
ably_error_t ably_rest_publish(ably_rest_client_t *client,
                                const char         *channel,
                                const char         *name,
                                const char         *data);

/* A message for REST publishing.  All fields may be NULL.
 * Set id for idempotent publishing (server deduplicates on the same id). */
typedef struct {
    const char *name;   /* event name; may be NULL */
    const char *data;   /* payload;    may be NULL */
    const char *id;     /* message id for idempotent publish; NULL = server-assigned */
} ably_rest_message_t;

/*
 * Publish a single message with an explicit ID for idempotent publishing.
 * id may be NULL (server assigns an ID).
 */
ably_error_t ably_rest_publish_with_id(ably_rest_client_t *client,
                                        const char         *channel,
                                        const char         *name,
                                        const char         *data,
                                        const char         *id);

/*
 * Publish multiple messages to one channel in a single HTTP request.
 * count must be >= 1.  Each message's id field is included if non-NULL.
 */
ably_error_t ably_rest_publish_batch(ably_rest_client_t        *client,
                                      const char                *channel,
                                      const ably_rest_message_t *messages,
                                      size_t                     count);

/* HTTP status code from the most recent request on this client. */
long ably_rest_last_http_status(const ably_rest_client_t *client);

/* ---------------------------------------------------------------------------
 * Server time
 * --------------------------------------------------------------------------- */

/*
 * Retrieve the Ably server time.
 * On success writes the server's Unix timestamp in milliseconds to *time_ms.
 * Returns ABLY_OK on HTTP 2xx.
 */
ably_error_t ably_rest_time(ably_rest_client_t *client, int64_t *time_ms);

/* ---------------------------------------------------------------------------
 * Channel history
 * --------------------------------------------------------------------------- */

/*
 * Retrieve historical messages for a channel.
 *
 *   channel      — channel name (not percent-encoded; library encodes it)
 *   limit        — max messages to retrieve; 0 = server default (100)
 *   direction    — "forwards" or "backwards" (NULL = server default)
 *   from_serial  — channelSerial to paginate from; NULL = no cursor
 *
 * On success writes a heap-allocated page to *page_out.
 * The caller must free it with ably_history_page_free().
 *
 * page->next_cursor is empty when this is the last page.
 * To page: pass page->next_cursor as from_serial in the next call.
 *
 * Returns ABLY_OK on HTTP 2xx, ABLY_ERR_HTTP on non-2xx.
 */
ably_error_t ably_rest_channel_history(ably_rest_client_t   *client,
                                        const char           *channel,
                                        int                   limit,
                                        const char           *direction,
                                        const char           *from_serial,
                                        ably_history_page_t **page_out);

/* Free a history page returned by ably_rest_channel_history(). */
void ably_history_page_free(ably_history_page_t *page);

/* ---------------------------------------------------------------------------
 * Channel status
 * --------------------------------------------------------------------------- */

/*
 * Retrieve the current occupancy and status for a channel.
 * Returns ABLY_OK on HTTP 2xx.
 */
ably_error_t ably_rest_channel_status(ably_rest_client_t    *client,
                                       const char            *channel,
                                       ably_channel_status_t *out);

/* ---------------------------------------------------------------------------
 * Stats
 * --------------------------------------------------------------------------- */

/*
 * Retrieve Ably account stats.
 *
 *   unit        — granularity: "minute", "hour", "day", "month" (NULL → "minute")
 *   start_ms    — start of time range (Unix ms); 0 = no lower bound
 *   end_ms      — end of time range   (Unix ms); 0 = no upper bound
 *   direction   — "forwards" or "backwards" (NULL → "backwards")
 *   limit       — max items to retrieve; 0 = server default (100)
 *   page_out    — receives a heap-allocated page; caller frees with ably_stats_page_free()
 *
 * page->next_cursor is empty when this is the last page.
 * To paginate: pass page->next_cursor as the path via a subsequent call
 * (the library detects that next_cursor starts with '/' and uses it directly).
 *
 * Returns ABLY_OK on HTTP 2xx, ABLY_ERR_HTTP on non-2xx.
 */
ably_error_t ably_rest_stats(ably_rest_client_t *client,
                              const char         *unit,
                              int64_t             start_ms,
                              int64_t             end_ms,
                              const char         *direction,
                              int                 limit,
                              ably_stats_page_t **page_out);

/* Free a stats page returned by ably_rest_stats(). */
void ably_stats_page_free(ably_stats_page_t *page);

#ifdef __cplusplus
}
#endif
#endif /* ABLY_REST_H */
