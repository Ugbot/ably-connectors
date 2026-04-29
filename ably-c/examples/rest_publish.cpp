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
 * rest_publish C++ example.
 *
 * Usage: ABLY_API_KEY=<key> ./rest_publish_cpp [channel [name [data]]]
 */

#include <ably/ably.hpp>
#include <iostream>
#include <cstdlib>

int main(int argc, char *argv[])
{
    const char *api_key = std::getenv("ABLY_API_KEY");
    if (!api_key) {
        std::cerr << "Set ABLY_API_KEY environment variable.\n";
        return 1;
    }

    std::string channel = argc > 1 ? argv[1] : "ably-c-test";
    std::string name    = argc > 2 ? argv[2] : "greeting";
    std::string data    = argc > 3 ? argv[3] : "Hello from ably-c C++!";

    try {
        ably::RestClient client(api_key);
        client.publish(channel, name, data);
        std::cout << "Published '" << name << "' to '" << channel << "'\n";
        std::cout << "HTTP status: " << client.lastHttpStatus() << "\n";
    } catch (const ably::Error &e) {
        std::cerr << "Ably error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
