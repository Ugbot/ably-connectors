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
 * realtime_subscribe_delta example: attach to a channel with VCDIFF delta
 * compression enabled and print each reconstructed full payload.
 *
 * Usage: ABLY_API_KEY=<appId.keyId:secret> ./realtime_subscribe_delta [channel]
 *
 * Enabling delta compression asks the Ably server to send successive messages
 * as VCDIFF diffs from the previous message rather than full payloads, reducing
 * bandwidth for high-frequency channels with slowly-changing data.  The client
 * library reconstructs the full payload transparently; subscribers always
 * receive the complete, decoded message.
 *
 * The program runs until it receives SIGINT/SIGTERM or the optional -n <count>
 * argument limit is reached.
 */

#include <ably/ably.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_sec(n) Sleep((DWORD)((n) * 1000))
#else
#  include <unistd.h>
#  define sleep_sec(n) sleep((unsigned)(n))
#endif

static volatile int g_quit = 0;
static volatile int g_message_count = 0;
static int          g_max_messages  = -1; /* -1 = unlimited */

static void handle_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void on_message(ably_channel_t       *channel,
                        const ably_message_t *msg,
                        void                 *user_data)
{
    (void)user_data;
    int n = ++g_message_count;
    printf("[%s] msg#%d name='%s' data='%s' id='%s'\n",
           ably_channel_name(channel),
           n,
           msg->name ? msg->name : "<null>",
           msg->data ? msg->data : "<null>",
           msg->id   ? msg->id   : "<null>");
    fflush(stdout);

    if (g_max_messages > 0 && n >= g_max_messages)
        g_quit = 1;
}

static void on_conn_state(ably_rt_client_t        *client,
                           ably_connection_state_t  new_state,
                           ably_connection_state_t  old_state,
                           ably_error_t             reason,
                           void                    *user_data)
{
    (void)old_state; (void)reason; (void)user_data;
    static const char *names[] = {
        "INITIALIZED","CONNECTING","CONNECTED","DISCONNECTED",
        "SUSPENDED","CLOSING","CLOSED","FAILED"
    };
    int nnames = (int)(sizeof(names)/sizeof(names[0]));
    printf("Connection: %s",
           (int)new_state < nnames ? names[(int)new_state] : "UNKNOWN");
    if (new_state == ABLY_CONN_CONNECTED)
        printf(" id=%s", ably_rt_client_connection_id(client));
    printf("\n");
    fflush(stdout);
}

static void on_chan_state(ably_channel_t      *channel,
                           ably_channel_state_t new_state,
                           ably_channel_state_t old_state,
                           ably_error_t         reason,
                           void                *user_data)
{
    (void)old_state; (void)reason; (void)user_data;
    static const char *names[] = {
        "INITIALIZED","ATTACHING","ATTACHED","DETACHING","DETACHED","FAILED"
    };
    int nnames = (int)(sizeof(names)/sizeof(names[0]));
    printf("Channel '%s': %s\n",
           ably_channel_name(channel),
           (int)new_state < nnames ? names[(int)new_state] : "UNKNOWN");
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Set ABLY_API_KEY environment variable.\n");
        return 1;
    }

    const char *channel_name = "ably-c-delta-test";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            g_max_messages = atoi(argv[++i]);
        } else {
            channel_name = argv[i];
        }
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

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

    /* Enable delta compression before attaching — this must be called first. */
    ably_error_t err = ably_channel_enable_delta(channel);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to enable delta: %s\n", ably_error_str(err));
        ably_rt_client_destroy(client);
        return 1;
    }

    ably_channel_set_state_cb(channel, on_chan_state, NULL);
    ably_channel_subscribe(channel, NULL, on_message, NULL);

    err = ably_rt_client_connect(client);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to start connection: %s\n", ably_error_str(err));
        ably_rt_client_destroy(client);
        return 1;
    }

    printf("Waiting for connection...\n");
    while (!g_quit) {
        ably_connection_state_t st = ably_rt_client_state(client);
        if (st == ABLY_CONN_CONNECTED) break;
        if (st == ABLY_CONN_FAILED) {
            fprintf(stderr, "Connection failed.\n");
            goto done;
        }
        sleep_sec(1);
    }
    if (g_quit) goto done;

    err = ably_channel_attach(channel);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to attach channel: %s\n", ably_error_str(err));
        goto done;
    }

    printf("Subscribed to '%s' with delta compression. Press Ctrl-C to exit.\n",
           channel_name);

    while (!g_quit) {
        sleep_sec(1);
    }

done:
    printf("\nReceived %d message(s). Shutting down...\n", g_message_count);
    ably_rt_client_close(client, 5000);
    ably_rt_client_destroy(client);
    return 0;
}
