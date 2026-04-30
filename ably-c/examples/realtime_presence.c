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
 * Example: realtime presence using the ably-c C API.
 *
 * Two clients:
 *  - A "publisher" that enters presence on a channel.
 *  - A "subscriber" that subscribes to presence events and reads the member list.
 *
 * Usage:
 *   ABLY_API_KEY=appId.keyId:secret ./realtime_presence
 */

#include <ably/ably_realtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((ms) * 1000u)
#endif

/* Poll until the client reaches CONNECTED (or timeout in ms). */
static int wait_connected(ably_rt_client_t *client, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (ably_rt_client_state(client) == ABLY_CONN_CONNECTED) return 0;
        sleep_ms(50);
        waited += 50;
    }
    return -1;
}

/* ---- presence subscriber callback --------------------------------------- */

static void on_presence(ably_channel_t *ch, const ably_presence_message_t *msg,
                         void *user_data)
{
    (void)ch; (void)user_data;

    const char *action_str = "UNKNOWN";
    switch (msg->action) {
    case ABLY_PRESENCE_ENTER:   action_str = "ENTER";   break;
    case ABLY_PRESENCE_LEAVE:   action_str = "LEAVE";   break;
    case ABLY_PRESENCE_UPDATE:  action_str = "UPDATE";  break;
    case ABLY_PRESENCE_PRESENT: action_str = "PRESENT"; break;
    default: break;
    }
    printf("[presence] %-8s  clientId=%-16s  data=%s\n",
           action_str, msg->client_id, msg->data);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key || !*api_key) {
        fprintf(stderr, "Set ABLY_API_KEY=appId.keyId:secret\n");
        return 1;
    }

    const char *channel_name = "presence-demo";

    /* ---- subscriber client ---- */
    ably_rt_client_t *sub_client = ably_rt_client_create(api_key, NULL, NULL);
    if (!sub_client) { fprintf(stderr, "failed to create subscriber client\n"); return 1; }

    ably_channel_t *sub_ch = ably_rt_channel_get(sub_client, channel_name);
    if (!sub_ch) { fprintf(stderr, "failed to get subscriber channel\n"); return 1; }

    ably_channel_presence_subscribe(sub_ch, on_presence, NULL);

    ably_rt_client_connect(sub_client);
    if (wait_connected(sub_client, 10000) < 0) {
        fprintf(stderr, "subscriber did not connect in time\n");
        return 1;
    }
    ably_channel_attach(sub_ch);

    /* ---- publisher client ---- */
    ably_rt_client_t *pub_client = ably_rt_client_create(api_key, NULL, NULL);
    if (!pub_client) { fprintf(stderr, "failed to create publisher client\n"); return 1; }

    ably_channel_t *pub_ch = ably_rt_channel_get(pub_client, channel_name);
    if (!pub_ch) { fprintf(stderr, "failed to get publisher channel\n"); return 1; }

    ably_rt_client_connect(pub_client);
    if (wait_connected(pub_client, 10000) < 0) {
        fprintf(stderr, "publisher did not connect in time\n");
        return 1;
    }
    ably_channel_attach(pub_ch);

    /* Wait for both channels to reach ATTACHED. */
    sleep_ms(2000);

    /* Enter presence. */
    printf("[main] entering presence as 'alice'\n");
    ably_channel_presence_enter(pub_ch, "alice", "hello from alice");
    sleep_ms(1000);

    /* Update data. */
    printf("[main] updating presence data\n");
    ably_channel_presence_update(pub_ch, "updated data");
    sleep_ms(1000);

    /* Print current member list. */
    ably_presence_message_t members[16];
    int total = 0;
    int n = ably_channel_presence_get_members(sub_ch, members, 16, &total);
    printf("[main] present members (%d total, %d returned):\n", total, n);
    for (int i = 0; i < n; i++) {
        printf("  - clientId=%s  data=%s\n",
               members[i].client_id, members[i].data);
    }

    /* Leave presence. */
    printf("[main] leaving presence\n");
    ably_channel_presence_leave(pub_ch, "goodbye");
    sleep_ms(1000);

    /* Clean up. */
    ably_rt_client_close(pub_client, 5000);
    ably_rt_client_destroy(pub_client);

    ably_rt_client_close(sub_client, 5000);
    ably_rt_client_destroy(sub_client);

    return 0;
}
