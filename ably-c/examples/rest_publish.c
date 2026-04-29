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
 * rest_publish example: publish one message via the Ably REST API.
 *
 * Usage: ABLY_API_KEY=<appId.keyId:secret> ./rest_publish [channel] [event] [data]
 */

#include <ably/ably.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Set ABLY_API_KEY environment variable.\n");
        return 1;
    }

    const char *channel = argc > 1 ? argv[1] : "ably-c-test";
    const char *event   = argc > 2 ? argv[2] : "greeting";
    const char *data    = argc > 3 ? argv[3] : "Hello from ably-c!";

    ably_set_log_level(ABLY_LOG_INFO);

    ably_rest_client_t *client = ably_rest_client_create(api_key, NULL, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create REST client.\n");
        return 1;
    }

    printf("Publishing to channel '%s': event='%s' data='%s'\n",
           channel, event, data);

    ably_error_t err = ably_rest_publish(client, channel, event, data);
    if (err == ABLY_OK) {
        printf("Published successfully (HTTP %ld)\n",
               ably_rest_last_http_status(client));
    } else {
        fprintf(stderr, "Publish failed: %s (HTTP %ld)\n",
                ably_error_str(err), ably_rest_last_http_status(client));
    }

    ably_rest_client_destroy(client);
    return err == ABLY_OK ? 0 : 1;
}
