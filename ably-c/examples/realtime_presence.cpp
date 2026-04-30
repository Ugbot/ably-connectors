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
 * Example: realtime presence using the ably-c C++ wrapper.
 *
 * Usage:
 *   ABLY_API_KEY=appId.keyId:secret ./realtime_presence_cpp
 */

#include <ably/ably.hpp>

#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>

static const char *action_name(ably_presence_action_t a) {
    switch (a) {
    case ABLY_PRESENCE_ENTER:   return "ENTER";
    case ABLY_PRESENCE_LEAVE:   return "LEAVE";
    case ABLY_PRESENCE_UPDATE:  return "UPDATE";
    case ABLY_PRESENCE_PRESENT: return "PRESENT";
    default:                    return "UNKNOWN";
    }
}

int main() {
    const char *api_key = std::getenv("ABLY_API_KEY");
    if (!api_key || !*api_key) {
        std::cerr << "Set ABLY_API_KEY=appId.keyId:secret\n";
        return 1;
    }

    constexpr const char *channel_name = "presence-demo-cpp";

    /* ---- subscriber ---- */
    ably::RealtimeClient sub_client(api_key);
    sub_client.connect();
    if (!sub_client.waitConnected(std::chrono::seconds(10))) {
        std::cerr << "subscriber did not connect in time\n";
        return 1;
    }

    auto &sub_ch = sub_client.channel(channel_name);
    sub_ch.attach();
    sub_ch.subscribePresence([](const ably_presence_message_t &msg) {
        std::cout << "[presence] " << action_name(msg.action)
                  << "  clientId=" << msg.client_id
                  << "  data=" << msg.data << "\n";
    });

    /* ---- publisher ---- */
    ably::RealtimeClient pub_client(api_key);
    pub_client.connect();
    if (!pub_client.waitConnected(std::chrono::seconds(10))) {
        std::cerr << "publisher did not connect in time\n";
        return 1;
    }

    auto &pub_ch = pub_client.channel(channel_name);
    pub_ch.attach();

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "[main] entering presence as 'bob'\n";
    pub_ch.enterPresence("bob", "hello from bob");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "[main] updating presence\n";
    pub_ch.updatePresence("updated data");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    /* Member list. */
    auto members = sub_ch.getPresenceMembers();
    std::cout << "[main] present members (" << members.size() << "):\n";
    for (const auto &m : members) {
        std::cout << "  - clientId=" << m.client_id
                  << "  data=" << m.data << "\n";
    }

    std::cout << "[main] leaving presence\n";
    pub_ch.leavePresence("bob", "goodbye");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    pub_client.close();
    sub_client.close();
    return 0;
}
