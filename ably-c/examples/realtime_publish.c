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
 * realtime_publish example: connect, attach a channel, and publish N messages.
 *
 * Usage: ABLY_API_KEY=<appId.keyId:secret> ./realtime_publish [channel] [event] [data] [count]
 */

#include <ably/ably.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_sec(n) Sleep((DWORD)((n) * 1000))
#else
#  include <unistd.h>
#  define sleep_sec(n) sleep((unsigned)(n))
#endif

static void on_conn_state(ably_rt_client_t        *client,
                           ably_connection_state_t  new_state,
                           ably_connection_state_t  old_state,
                           ably_error_t             reason,
                           void                    *user_data)
{
    (void)client; (void)old_state; (void)reason; (void)user_data;
    const char *names[] = {
        "INITIALIZED","CONNECTING","CONNECTED","DISCONNECTED",
        "SUSPENDED","CLOSING","CLOSED","FAILED"
    };
    int n = (int)(sizeof(names)/sizeof(names[0]));
    printf("Connection: %s\n", (int)new_state < n ? names[(int)new_state] : "UNKNOWN");
    fflush(stdout);
}

static void on_chan_state(ably_channel_t      *channel,
                           ably_channel_state_t new_state,
                           ably_channel_state_t old_state,
                           ably_error_t         reason,
                           void                *user_data)
{
    (void)old_state; (void)reason; (void)user_data;
    const char *names[] = {
        "INITIALIZED","ATTACHING","ATTACHED","DETACHING","DETACHED","FAILED"
    };
    int n = (int)(sizeof(names)/sizeof(names[0]));
    printf("Channel '%s': %s\n",
           ably_channel_name(channel),
           (int)new_state < n ? names[(int)new_state] : "UNKNOWN");
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Set ABLY_API_KEY environment variable.\n");
        return 1;
    }

    const char *channel_name = argc > 1 ? argv[1] : "ably-c-test";
    const char *event        = argc > 2 ? argv[2] : "greeting";
    const char *data         = argc > 3 ? argv[3] : "Hello from ably-c!";
    int         count        = argc > 4 ? atoi(argv[4]) : 5;

    ably_set_log_level(ABLY_LOG_INFO);

    ably_rt_client_t *client = ably_rt_client_create(api_key, NULL, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create real-time client.\n");
        return 1;
    }

    ably_rt_client_set_conn_state_cb(client, on_conn_state, NULL);

    ably_channel_t *channel = ably_rt_channel_get(client, channel_name);
    if (!channel) {
        fprintf(stderr, "Failed to get channel '%s'.\n", channel_name);
        ably_rt_client_destroy(client);
        return 1;
    }
    ably_channel_set_state_cb(channel, on_chan_state, NULL);

    ably_error_t err = ably_rt_client_connect(client);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to start connection: %s\n", ably_error_str(err));
        ably_rt_client_destroy(client);
        return 1;
    }

    /* Wait for CONNECTED. */
    for (int i = 0; i < 30; i++) {
        ably_connection_state_t st = ably_rt_client_state(client);
        if (st == ABLY_CONN_CONNECTED) break;
        if (st == ABLY_CONN_FAILED) {
            fprintf(stderr, "Connection failed.\n");
            ably_rt_client_destroy(client);
            return 1;
        }
        sleep_sec(1);
    }

    if (ably_rt_client_state(client) != ABLY_CONN_CONNECTED) {
        fprintf(stderr, "Timed out waiting for connection.\n");
        ably_rt_client_destroy(client);
        return 1;
    }

    err = ably_channel_attach(channel);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to attach: %s\n", ably_error_str(err));
        ably_rt_client_destroy(client);
        return 1;
    }

    /* Wait for ATTACHED. */
    for (int i = 0; i < 10; i++) {
        if (ably_channel_state(channel) == ABLY_CHAN_ATTACHED) break;
        sleep_sec(1);
    }
    if (ably_channel_state(channel) != ABLY_CHAN_ATTACHED) {
        fprintf(stderr, "Timed out waiting for channel ATTACHED.\n");
        ably_rt_client_destroy(client);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        char payload[256];
        snprintf(payload, sizeof(payload), "%s #%d", data, i + 1);
        err = ably_channel_publish(channel, event, payload);
        if (err != ABLY_OK) {
            fprintf(stderr, "Publish %d failed: %s\n", i + 1, ably_error_str(err));
        } else {
            printf("Published: event='%s' data='%s'\n", event, payload);
        }
    }

    /* Brief wait for any in-flight frames to drain. */
    sleep_sec(2);

    ably_rt_client_close(client, 5000);
    ably_rt_client_destroy(client);
    return err == ABLY_OK ? 0 : 1;
}
