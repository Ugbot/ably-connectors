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
 * realtime_subscribe example: attach to a channel and print incoming messages.
 *
 * Usage: ABLY_API_KEY=<appId.keyId:secret> ./realtime_subscribe [channel]
 *
 * The program runs until Ctrl-C is pressed.
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
    printf("[%s] name='%s' data='%s' id='%s'\n",
           ably_channel_name(channel),
           msg->name      ? msg->name      : "<null>",
           msg->data      ? msg->data      : "<null>",
           msg->id        ? msg->id        : "<null>");
    fflush(stdout);
}

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
    printf("Connection: %s\n",
           (int)new_state < n ? names[(int)new_state] : "UNKNOWN");
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

    ably_channel_set_state_cb(channel, on_chan_state, NULL);
    ably_channel_subscribe(channel, NULL, on_message, NULL);

    ably_error_t err = ably_rt_client_connect(client);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to start connection: %s\n", ably_error_str(err));
        ably_rt_client_destroy(client);
        return 1;
    }

    /* Wait until CONNECTED then attach the channel. */
    printf("Waiting for connection...\n");
    while (!g_quit) {
        ably_connection_state_t st = ably_rt_client_state(client);
        if (st == ABLY_CONN_CONNECTED) break;
        if (st == ABLY_CONN_FAILED)    { fprintf(stderr, "Connection failed.\n"); goto done; }
        sleep_sec(1);
    }
    if (g_quit) goto done;

    err = ably_channel_attach(channel);
    if (err != ABLY_OK) {
        fprintf(stderr, "Failed to attach channel: %s\n", ably_error_str(err));
        goto done;
    }

    printf("Subscribed to '%s'. Press Ctrl-C to exit.\n", channel_name);

    while (!g_quit) {
        sleep_sec(1);
    }

done:
    printf("\nShutting down...\n");
    ably_rt_client_close(client, 5000);
    ably_rt_client_destroy(client);
    return 0;
}
