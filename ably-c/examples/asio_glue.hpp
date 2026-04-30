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
 * Boost.Asio event-loop integration for ably-c.
 *
 * Header-only C++17 adapter.  Drop this file into your project and use
 * AblyAsioSession to drive an ably-c realtime client from within an existing
 * Asio io_context without spawning the library's internal service thread.
 *
 * Key ably-c APIs used:
 *
 *   ably_rt_client_connect_async(client)
 *     — TLS+WebSocket handshake; no internal service thread.
 *
 *   ably_rt_client_fd(client)
 *     — Underlying socket fd wrapped in an Asio stream_descriptor.
 *
 *   ably_rt_step(client, timeout_ms)
 *     — One poll iteration: drain one outbound + receive one inbound.
 *       Returns 1 (work done), 0 (timeout), -1 (error/disconnect).
 *
 * Minimal usage example (see main() at the bottom of this file):
 *
 *   boost::asio::io_context ioc;
 *   AblyAsioSession session(ioc, api_key, "my-channel");
 *   session.connect();
 *   ioc.run();
 *
 * Requires Boost >= 1.70 and a C++17 compiler.
 */

#pragma once

#include <ably/ably_realtime.h>

#include <boost/asio.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <iostream>
#include <cassert>

namespace asio = boost::asio;
using boost::system::error_code;

/* --------------------------------------------------------------------------
 * AblyAsioSession
 *
 * Owns an ably_rt_client_t and a channel.  Drives ably_rt_step() from an
 * Asio async-read loop that wakes whenever the socket becomes readable.
 * -------------------------------------------------------------------------- */
class AblyAsioSession : public std::enable_shared_from_this<AblyAsioSession> {
public:
    using MessageCb = std::function<void(const ably_message_t &)>;

    /* Factory: always use make_shared to ensure shared_from_this works. */
    static std::shared_ptr<AblyAsioSession>
    create(asio::io_context &ioc, const std::string &api_key,
           const std::string &channel_name)
    {
        return std::shared_ptr<AblyAsioSession>(
            new AblyAsioSession(ioc, api_key, channel_name));
    }

    ~AblyAsioSession()
    {
        if (client_) ably_rt_client_destroy(client_);
    }

    /* Register a message callback (call before connect()). */
    void onMessage(MessageCb cb) { msg_cb_ = std::move(cb); }

    /* Connect asynchronously (non-blocking handshake then arm Asio poll). */
    void connect()
    {
        ably_error_t err = ably_rt_client_connect_async(client_);
        if (err != ABLY_OK) {
            scheduleReconnect();
            return;
        }

        ably_channel_attach(channel_);
        reconnect_ms_ = 1000;

        int fd = ably_rt_client_fd(client_);
        assert(fd >= 0);

        sd_ = std::make_unique<asio::posix::stream_descriptor>(ioc_, fd);
        doRead();
    }

private:
    AblyAsioSession(asio::io_context &ioc, const std::string &api_key,
                    const std::string &channel_name)
        : ioc_(ioc), reconnect_ms_(1000)
    {
        client_ = ably_rt_client_create(api_key.c_str(), nullptr, nullptr);
        if (!client_) throw std::runtime_error("ably_rt_client_create failed");

        channel_ = ably_rt_channel_get(client_, channel_name.c_str());
        if (!channel_) throw std::runtime_error("ably_rt_client_get_channel failed");

        /* Route inbound messages to our callback. */
        ably_channel_subscribe(channel_, nullptr,
            [](ably_channel_t *, const ably_message_t *msg, void *ud) {
                auto *self = static_cast<AblyAsioSession *>(ud);
                if (self->msg_cb_) self->msg_cb_(*msg);
            }, this);
    }

    void doRead()
    {
        auto self = shared_from_this();
        sd_->async_wait(asio::posix::stream_descriptor::wait_read,
            [self](const error_code &ec) {
                if (ec) {
                    std::cerr << "[ably/asio] poll error: " << ec.message()
                              << " — reconnecting\n";
                    self->scheduleReconnect();
                    return;
                }

                int rc = ably_rt_step(self->client_, 0);
                if (rc < 0) {
                    std::cerr << "[ably/asio] connection dropped — reconnecting\n";
                    self->scheduleReconnect();
                    return;
                }

                self->doRead();
            });
    }

    void scheduleReconnect()
    {
        /* Release the stream_descriptor before reconnecting so the old fd is
         * not double-watched.  The fd is owned by the ws_client. */
        sd_.reset();

        auto self    = shared_from_this();
        auto timer   = std::make_shared<asio::steady_timer>(ioc_);
        timer->expires_after(std::chrono::milliseconds(reconnect_ms_));
        reconnect_ms_ = reconnect_ms_ < 30000 ? reconnect_ms_ * 2 : 30000;

        timer->async_wait([self, timer](const error_code &ec) {
            if (!ec) self->connect();
        });
    }

    asio::io_context                                      &ioc_;
    ably_rt_client_t                                      *client_  = nullptr;
    ably_channel_t                                        *channel_ = nullptr;
    std::unique_ptr<asio::posix::stream_descriptor>        sd_;
    MessageCb                                              msg_cb_;
    int                                                    reconnect_ms_;
};

/* --------------------------------------------------------------------------
 * Standalone example main (compiled when ABLY_ASIO_MAIN is defined).
 * -------------------------------------------------------------------------- */
#ifdef ABLY_ASIO_MAIN
#include <cstdlib>

int main(int argc, char **argv)
{
    const char *api_key = std::getenv("ABLY_API_KEY");
    if (!api_key || !*api_key) {
        std::cerr << "Set ABLY_API_KEY=appId.keyId:secret\n";
        return 1;
    }
    const std::string channel_name = (argc > 1) ? argv[1] : "asio-demo";

    asio::io_context ioc;

    auto session = AblyAsioSession::create(ioc, api_key, channel_name);
    session->onMessage([](const ably_message_t &msg) {
        std::cout << "[msg] name=" << (msg.name ? msg.name : "")
                  << "  data=" << (msg.data ? msg.data : "") << "\n";
    });
    session->connect();

    std::cout << "[asio] running io_context (Ctrl-C to exit)\n";
    ioc.run();
    return 0;
}
#endif /* ABLY_ASIO_MAIN */
