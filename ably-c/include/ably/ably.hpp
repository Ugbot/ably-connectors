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
 * ably.hpp — C++17 header-only binding for ably-c.
 *
 * Thin RAII wrappers around the C API.  Every method delegates to the
 * corresponding C function; no additional logic lives here.
 *
 * Requirements: C++17, link against libably.a.
 */

#pragma once

#include "ably.h"

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ably {

/* ---------------------------------------------------------------------------
 * Error
 * --------------------------------------------------------------------------- */

class Error : public std::runtime_error {
public:
    explicit Error(ably_error_t code)
        : std::runtime_error(ably_error_str(code)), code_(code) {}

    ably_error_t code() const noexcept { return code_; }

private:
    ably_error_t code_;
};

inline void check(ably_error_t err)
{
    if (err != ABLY_OK) throw Error(err);
}

/* ---------------------------------------------------------------------------
 * Message
 * --------------------------------------------------------------------------- */

struct Message {
    std::string id;
    std::string name;
    std::string data;
    std::string client_id;
    int64_t     timestamp{0};

    static Message from_c(const ably_message_t *m)
    {
        Message r;
        if (m->id)        r.id        = m->id;
        if (m->name)      r.name      = m->name;
        if (m->data)      r.data      = m->data;
        if (m->client_id) r.client_id = m->client_id;
        r.timestamp = m->timestamp;
        return r;
    }
};

/* ---------------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------------- */

struct RestOptions {
    std::string  rest_host;          /* default: "rest.ably.io" */
    uint16_t     port{443};
    int          timeout_ms{10000};
    int          tls_verify_peer{1};
    ably_encoding_t encoding{ABLY_ENCODING_JSON};
};

struct RealtimeOptions {
    std::string  realtime_host;      /* default: "realtime.ably.io" */
    uint16_t     port{443};
    ably_encoding_t encoding{ABLY_ENCODING_JSON};
    int          reconnect_initial_delay_ms{500};
    int          reconnect_max_delay_ms{60000};
    int          reconnect_max_attempts{-1};
    int          heartbeat_timeout_ms{35000};
    int          tls_verify_peer{1};
};

/* ---------------------------------------------------------------------------
 * Channel
 *
 * Owned by RealtimeClient.  Do not construct directly.
 * --------------------------------------------------------------------------- */

class Channel {
public:
    /* Non-copyable, non-movable — RealtimeClient hands out references. */
    Channel(const Channel &)            = delete;
    Channel &operator=(const Channel &) = delete;

    void setStateCallback(std::function<void(ably_channel_state_t, ably_channel_state_t, ably_error_t)> cb)
    {
        state_cb_ = std::move(cb);
        ably_channel_set_state_cb(raw_, &Channel::dispatch_state_cb, this);
    }

    /* Returns a subscription token. */
    int subscribe(std::optional<std::string> name_filter,
                  std::function<void(const Message &)> cb)
    {
        auto *slot = new SubSlot{std::move(cb)};
        const char *filter = nullptr;
        if (name_filter) filter = name_filter->c_str();
        int token = ably_channel_subscribe(raw_, filter, &Channel::dispatch_msg_cb, slot);
        if (token <= 0) {
            delete slot;
            throw Error(static_cast<ably_error_t>(token));
        }
        sub_slots_[token] = slot;
        return token;
    }

    void unsubscribe(int token)
    {
        check(ably_channel_unsubscribe(raw_, token));
        auto it = sub_slots_.find(token);
        if (it != sub_slots_.end()) {
            delete it->second;
            sub_slots_.erase(it);
        }
    }

    void attach()       { check(ably_channel_attach(raw_)); }
    void detach()       { check(ably_channel_detach(raw_)); }
    void enableDelta()  { check(ably_channel_enable_delta(raw_)); }

    void publish(std::string_view name, std::string_view data)
    {
        check(ably_channel_publish(raw_,
            std::string(name).c_str(),
            std::string(data).c_str()));
    }

    ably_channel_state_t state() const { return ably_channel_state(raw_); }
    std::string_view     name()  const { return ably_channel_name(raw_); }

    /* Presence API */

    int subscribePresence(std::function<void(const ably_presence_message_t &)> cb)
    {
        auto *slot = new PresSlot{std::move(cb)};
        int token = ably_channel_presence_subscribe(raw_, &Channel::dispatch_pres_cb, slot);
        if (token <= 0) { delete slot; return token; }
        pres_slots_[token] = slot;
        return token;
    }

    void unsubscribePresence(int token)
    {
        ably_channel_presence_unsubscribe(raw_, token);
        auto it = pres_slots_.find(token);
        if (it != pres_slots_.end()) { delete it->second; pres_slots_.erase(it); }
    }

    void enterPresence(std::string_view client_id, std::string_view data = {})
    {
        check(ably_channel_presence_enter(raw_,
            std::string(client_id).c_str(),
            data.empty() ? nullptr : std::string(data).c_str()));
    }

    void leavePresence(std::string_view client_id, std::string_view data = {})
    {
        (void)client_id; /* client_id is stored in pres->own_client_id */
        check(ably_channel_presence_leave(raw_,
            data.empty() ? nullptr : std::string(data).c_str()));
    }

    void updatePresence(std::string_view data = {})
    {
        check(ably_channel_presence_update(raw_,
            data.empty() ? nullptr : std::string(data).c_str()));
    }

    std::vector<ably_presence_message_t> getPresenceMembers()
    {
        int total = 0;
        ably_channel_presence_get_members(raw_, nullptr, 0, &total);
        if (total <= 0) return {};
        std::vector<ably_presence_message_t> v(static_cast<size_t>(total));
        ably_channel_presence_get_members(raw_, v.data(),
                                          static_cast<int>(v.size()), &total);
        v.resize(static_cast<size_t>(total));
        return v;
    }

    ably_channel_t *raw() const noexcept { return raw_; }

private:
    friend class RealtimeClient;

    explicit Channel(ably_channel_t *raw) : raw_(raw) {}

    ~Channel()
    {
        for (auto &[tok, slot] : sub_slots_) {
            (void)tok;
            delete slot;
        }
    }

    static void dispatch_state_cb(ably_channel_t      *,
                                   ably_channel_state_t  ns,
                                   ably_channel_state_t  os,
                                   ably_error_t          reason,
                                   void                 *ud)
    {
        auto *self = static_cast<Channel *>(ud);
        if (self->state_cb_) self->state_cb_(ns, os, reason);
    }

    struct SubSlot {
        std::function<void(const Message &)> cb;
    };

    static void dispatch_msg_cb(ably_channel_t       *,
                                 const ably_message_t *msg,
                                 void                 *ud)
    {
        auto *slot = static_cast<SubSlot *>(ud);
        slot->cb(Message::from_c(msg));
    }

    struct PresSlot {
        std::function<void(const ably_presence_message_t &)> cb;
    };

    static void dispatch_pres_cb(ably_channel_t             *,
                                  const ably_presence_message_t *msg,
                                  void                          *ud)
    {
        auto *slot = static_cast<PresSlot *>(ud);
        slot->cb(*msg);
    }

    ably_channel_t  *raw_;
    std::function<void(ably_channel_state_t, ably_channel_state_t, ably_error_t)> state_cb_;
    std::unordered_map<int, SubSlot  *> sub_slots_;
    std::unordered_map<int, PresSlot *> pres_slots_;
};

/* ---------------------------------------------------------------------------
 * RestClient
 * --------------------------------------------------------------------------- */

class RestClient {
public:
    explicit RestClient(std::string_view api_key,
                         RestOptions opts = {})
    {
        ably_rest_options_t copts;
        ably_rest_options_init(&copts);
        if (!opts.rest_host.empty()) copts.rest_host    = opts.rest_host.c_str();
        if (opts.port)               copts.port          = opts.port;
        copts.timeout_ms      = opts.timeout_ms;
        copts.tls_verify_peer = opts.tls_verify_peer;
        copts.encoding        = opts.encoding;

        raw_ = ably_rest_client_create(std::string(api_key).c_str(), &copts, nullptr);
        if (!raw_) throw Error(ABLY_ERR_NOMEM);
    }

    ~RestClient() { ably_rest_client_destroy(raw_); }

    RestClient(const RestClient &)            = delete;
    RestClient &operator=(const RestClient &) = delete;

    void publish(std::string_view channel, std::string_view name, std::string_view data)
    {
        check(ably_rest_publish(raw_,
            std::string(channel).c_str(),
            std::string(name).c_str(),
            std::string(data).c_str()));
    }

    void publishBatch(std::string_view channel, const std::vector<Message> &msgs)
    {
        std::vector<ably_rest_message_t> raw_msgs;
        raw_msgs.reserve(msgs.size());
        for (const auto &m : msgs) {
            ably_rest_message_t rm{};
            rm.name = m.name.c_str();
            rm.data = m.data.c_str();
            raw_msgs.push_back(rm);
        }
        check(ably_rest_publish_batch(raw_,
            std::string(channel).c_str(),
            raw_msgs.data(), raw_msgs.size()));
    }

    long lastHttpStatus() const { return ably_rest_last_http_status(raw_); }

    ably_rest_client_t *raw() const noexcept { return raw_; }

private:
    ably_rest_client_t *raw_;
};

/* ---------------------------------------------------------------------------
 * RealtimeClient
 * --------------------------------------------------------------------------- */

class RealtimeClient {
public:
    explicit RealtimeClient(std::string_view api_key,
                             RealtimeOptions opts = {})
    {
        ably_rt_options_t copts;
        ably_rt_options_init(&copts);
        if (!opts.realtime_host.empty()) copts.realtime_host = opts.realtime_host.c_str();
        if (opts.port) copts.port = opts.port;
        copts.encoding                   = opts.encoding;
        copts.reconnect_initial_delay_ms = opts.reconnect_initial_delay_ms;
        copts.reconnect_max_delay_ms     = opts.reconnect_max_delay_ms;
        copts.reconnect_max_attempts     = opts.reconnect_max_attempts;
        copts.heartbeat_timeout_ms       = opts.heartbeat_timeout_ms;
        copts.tls_verify_peer            = opts.tls_verify_peer;

        raw_ = ably_rt_client_create(std::string(api_key).c_str(), &copts, nullptr);
        if (!raw_) throw Error(ABLY_ERR_NOMEM);
    }

    ~RealtimeClient()
    {
        ably_rt_client_destroy(raw_);
        for (auto &[name, ch] : channels_) {
            (void)name;
            delete ch;
        }
    }

    RealtimeClient(const RealtimeClient &)            = delete;
    RealtimeClient &operator=(const RealtimeClient &) = delete;

    void onConnectionState(
        std::function<void(ably_connection_state_t, ably_connection_state_t, ably_error_t)> cb)
    {
        conn_state_cb_ = std::move(cb);
        ably_rt_client_set_conn_state_cb(raw_, &RealtimeClient::dispatch_conn_cb, this);
    }

    void connect() { check(ably_rt_client_connect(raw_)); }

    void close(std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        check(ably_rt_client_close(raw_, (int)timeout.count()));
    }

    ably_connection_state_t state() const { return ably_rt_client_state(raw_); }
    std::string connectionId() const { return ably_rt_client_connection_id(raw_); }

    /* Block until CONNECTED state or timeout elapses.
     * Returns true on success, false on timeout. */
    bool waitConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + timeout;
        while (clock::now() < deadline) {
            if (state() == ABLY_CONN_CONNECTED) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    /* Returns a reference valid for the lifetime of this client. */
    Channel &channel(std::string_view name)
    {
        std::string key(name);
        auto it = channels_.find(key);
        if (it != channels_.end()) return *it->second;

        ably_channel_t *raw_ch = ably_rt_channel_get(raw_, key.c_str());
        if (!raw_ch) throw Error(ABLY_ERR_CAPACITY);

        auto *ch = new Channel(raw_ch);
        channels_.emplace(key, ch);
        return *ch;
    }

    ably_rt_client_t *raw() const noexcept { return raw_; }

private:
    static void dispatch_conn_cb(ably_rt_client_t        *,
                                  ably_connection_state_t  ns,
                                  ably_connection_state_t  os,
                                  ably_error_t             reason,
                                  void                    *ud)
    {
        auto *self = static_cast<RealtimeClient *>(ud);
        if (self->conn_state_cb_) self->conn_state_cb_(ns, os, reason);
    }

    ably_rt_client_t *raw_;
    std::function<void(ably_connection_state_t, ably_connection_state_t, ably_error_t)> conn_state_cb_;
    std::unordered_map<std::string, Channel *> channels_;
};

} // namespace ably
