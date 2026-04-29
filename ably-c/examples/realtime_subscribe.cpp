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
 * realtime_subscribe C++ example.
 *
 * Usage: ABLY_API_KEY=<key> ./realtime_subscribe_cpp [channel]
 *
 * Runs until Ctrl-C.
 */

#include <ably/ably.hpp>
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <chrono>

static volatile bool g_quit = false;

static void handle_signal(int) { g_quit = true; }

int main(int argc, char *argv[])
{
    const char *api_key = std::getenv("ABLY_API_KEY");
    if (!api_key) {
        std::cerr << "Set ABLY_API_KEY environment variable.\n";
        return 1;
    }

    std::string channel_name = argc > 1 ? argv[1] : "ably-c-test";

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    ably_set_log_level(ABLY_LOG_INFO);

    try {
        ably::RealtimeClient client(api_key);

        client.onConnectionState([](ably_connection_state_t ns,
                                    ably_connection_state_t /*os*/,
                                    ably_error_t            /*reason*/) {
            static const char *names[] = {
                "INITIALIZED","CONNECTING","CONNECTED","DISCONNECTED",
                "SUSPENDED","CLOSING","CLOSED","FAILED"
            };
            int n = (int)(sizeof(names)/sizeof(names[0]));
            std::cout << "Connection: "
                      << ((int)ns < n ? names[(int)ns] : "UNKNOWN") << "\n";
        });

        auto &ch = client.channel(channel_name);

        ch.setStateCallback([&channel_name](ably_channel_state_t ns,
                                             ably_channel_state_t /*os*/,
                                             ably_error_t         /*reason*/) {
            static const char *names[] = {
                "INITIALIZED","ATTACHING","ATTACHED","DETACHING","DETACHED","FAILED"
            };
            int n = (int)(sizeof(names)/sizeof(names[0]));
            std::cout << "Channel '" << channel_name << "': "
                      << ((int)ns < n ? names[(int)ns] : "UNKNOWN") << "\n";
        });

        ch.subscribe(std::nullopt, [&channel_name](const ably::Message &msg) {
            std::cout << "[" << channel_name << "] "
                      << "name='" << msg.name << "' "
                      << "data='" << msg.data << "' "
                      << "id='"   << msg.id   << "'\n";
            std::cout.flush();
        });

        client.connect();

        std::cout << "Waiting for connection...\n";
        while (!g_quit && client.state() != ABLY_CONN_CONNECTED) {
            if (client.state() == ABLY_CONN_FAILED) {
                std::cerr << "Connection failed.\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (g_quit) return 0;

        ch.attach();
        std::cout << "Subscribed to '" << channel_name << "'. Press Ctrl-C to exit.\n";

        while (!g_quit) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "\nShutting down...\n";
        client.close();

    } catch (const ably::Error &e) {
        std::cerr << "Ably error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
