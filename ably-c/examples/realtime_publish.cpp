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
 * realtime_publish C++ example.
 *
 * Usage: ABLY_API_KEY=<key> ./realtime_publish_cpp [channel [name [data [count]]]]
 *
 * Connects, attaches, publishes <count> messages, then closes.
 */

#include <ably/ably.hpp>
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>

int main(int argc, char *argv[])
{
    const char *api_key = std::getenv("ABLY_API_KEY");
    if (!api_key) {
        std::cerr << "Set ABLY_API_KEY environment variable.\n";
        return 1;
    }

    std::string channel_name = argc > 1 ? argv[1] : "ably-c-test";
    std::string event_name   = argc > 2 ? argv[2] : "message";
    std::string data         = argc > 3 ? argv[3] : "hello from ably-c C++";
    int         count        = argc > 4 ? std::atoi(argv[4]) : 1;
    if (count <= 0) count = 1;

    ably_set_log_level(ABLY_LOG_WARN);

    try {
        ably::RealtimeClient client(api_key);
        client.connect();

        while (client.state() != ABLY_CONN_CONNECTED) {
            if (client.state() == ABLY_CONN_FAILED) {
                std::cerr << "Connection failed.\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        auto &ch = client.channel(channel_name);
        ch.attach();

        while (ch.state() != ABLY_CHAN_ATTACHED) {
            if (ch.state() == ABLY_CHAN_FAILED) {
                std::cerr << "Channel attach failed.\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        for (int i = 0; i < count; ++i) {
            ch.publish(event_name, data);
            std::cout << "Published [" << (i + 1) << "/" << count << "] "
                      << "'" << event_name << "': " << data << "\n";
            if (count > 1 && i < count - 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

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
