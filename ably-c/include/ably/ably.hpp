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

/* ErrorInfo wraps ably_error_info_t. */
struct ErrorInfo {
    int         ably_code{0};
    std::string message;

    static ErrorInfo from_c(const ably_error_info_t *e)
    {
        ErrorInfo r;
        if (!e) return r;
        r.ably_code = e->ably_code;
        r.message   = e->message;
        return r;
    }
    bool ok() const { return ably_code == 0; }
};

/* PresenceMessage wraps ably_presence_message_t. */
struct PresenceMessage {
    ably_presence_action_t action{ABLY_PRESENCE_ABSENT};
    std::string            client_id;
    std::string            connection_id;
    std::string            data;
    uint64_t               timestamp{0};

    static PresenceMessage from_c(const ably_presence_message_t &m)
    {
        PresenceMessage r;
        r.action        = m.action;
        r.client_id     = m.client_id;
        r.connection_id = m.connection_id;
        r.data          = m.data;
        r.timestamp     = m.timestamp;
        return r;
    }
};

/* HistoryPage wraps a heap-allocated ably_history_page_t. */
class HistoryPage {
public:
    explicit HistoryPage(ably_history_page_t *p) : page_(p) {}
    ~HistoryPage() { if (page_) ably_history_page_free(page_); }

    HistoryPage(HistoryPage &&o) noexcept : page_(o.page_) { o.page_ = nullptr; }
    HistoryPage &operator=(HistoryPage &&o) noexcept
    {
        if (page_) ably_history_page_free(page_);
        page_ = o.page_; o.page_ = nullptr;
        return *this;
    }
    HistoryPage(const HistoryPage &) = delete;
    HistoryPage &operator=(const HistoryPage &) = delete;

    size_t size() const { return page_ ? page_->count : 0; }
    bool   empty() const { return size() == 0; }
    std::string nextCursor() const
    {
        if (page_ && page_->next_cursor[0]) return page_->next_cursor;
        return {};
    }
    Message operator[](size_t i) const { return Message::from_c(&page_->items[i]); }

private:
    ably_history_page_t *page_;
};

/* ChannelStatus wraps ably_channel_status_t. */
struct ChannelStatus {
    std::string      name;
    bool             is_active{false};
    ably_occupancy_t occupancy{};

    static ChannelStatus from_c(const ably_channel_status_t &s)
    {
        ChannelStatus r;
        r.name      = s.name;
        r.is_active = s.is_active != 0;
        r.occupancy = s.occupancy;
        return r;
    }
};

/* PresencePage wraps a heap-allocated ably_presence_page_t. */
class PresencePage {
public:
    explicit PresencePage(ably_presence_page_t *p) : page_(p) {}
    ~PresencePage() { if (page_) ably_presence_page_free(page_); }

    PresencePage(PresencePage &&o) noexcept : page_(o.page_) { o.page_ = nullptr; }
    PresencePage &operator=(PresencePage &&o) noexcept
    {
        if (page_) ably_presence_page_free(page_);
        page_ = o.page_; o.page_ = nullptr;
        return *this;
    }
    PresencePage(const PresencePage &) = delete;
    PresencePage &operator=(const PresencePage &) = delete;

    size_t size() const { return page_ ? page_->count : 0; }
    std::string nextCursor() const
    {
        if (page_ && page_->next_cursor[0]) return page_->next_cursor;
        return {};
    }
    PresenceMessage operator[](size_t i) const
    {
        return PresenceMessage::from_c(page_->items[i]);
    }

private:
    ably_presence_page_t *page_;
};

/* ChannelListPage wraps a heap-allocated ably_channel_list_page_t. */
class ChannelListPage {
public:
    explicit ChannelListPage(ably_channel_list_page_t *p) : page_(p) {}
    ~ChannelListPage() { if (page_) ably_channel_list_page_free(page_); }

    ChannelListPage(ChannelListPage &&o) noexcept : page_(o.page_) { o.page_ = nullptr; }
    ChannelListPage &operator=(ChannelListPage &&o) noexcept
    {
        if (page_) ably_channel_list_page_free(page_);
        page_ = o.page_; o.page_ = nullptr;
        return *this;
    }
    ChannelListPage(const ChannelListPage &) = delete;
    ChannelListPage &operator=(const ChannelListPage &) = delete;

    size_t size() const { return page_ ? page_->count : 0; }
    std::string nextCursor() const
    {
        if (page_ && page_->next_cursor[0]) return page_->next_cursor;
        return {};
    }
    ChannelStatus operator[](size_t i) const
    {
        return ChannelStatus::from_c(page_->items[i]);
    }

private:
    ably_channel_list_page_t *page_;
};

/* TokenDetails wraps ably_token_details_t. */
struct TokenDetails {
    std::string token;
    std::string key_name;
    int64_t     issued{0};
    int64_t     expires{0};
    std::string capability;
    std::string client_id;

    static TokenDetails from_c(const ably_token_details_t &d)
    {
        TokenDetails r;
        r.token      = d.token;
        r.key_name   = d.key_name;
        r.issued     = d.issued;
        r.expires    = d.expires;
        r.capability = d.capability;
        r.client_id  = d.client_id;
        return r;
    }
};

/* TokenParams wraps ably_token_params_t. */
struct TokenParams {
    std::string capability;  /* JSON capability; empty = all */
    std::string client_id;
    int64_t     ttl_ms{0};  /* 0 = server default */
};

/* RestResponse wraps ably_rest_response_t. */
struct RestResponse {
    long        http_status{0};
    std::string body;
    std::string next_cursor;

    static RestResponse from_c(const ably_rest_response_t &r)
    {
        RestResponse out;
        out.http_status  = r.http_status;
        if (r.body && r.body_len) out.body = std::string(r.body, r.body_len);
        if (r.next_cursor[0]) out.next_cursor = r.next_cursor;
        return out;
    }
};

struct RestOptions {
    std::string  rest_host;          /* default: "rest.ably.io" */
    uint16_t     port{443};
    int          timeout_ms{10000};
    int          tls_verify_peer{1};
    ably_encoding_t encoding{ABLY_ENCODING_JSON};
    std::string  token;              /* pre-obtained token for Bearer auth */
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
    std::string  client_id;
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

    void publishWithId(std::string_view name, std::string_view data, std::string_view id)
    {
        check(ably_channel_publish_with_id(raw_,
            std::string(name).c_str(),
            std::string(data).c_str(),
            std::string(id).c_str()));
    }

    void setRewind(int count) { ably_channel_set_rewind(raw_, count); }
    void setModes(uint32_t modes) { ably_channel_set_modes(raw_, modes); }
    uint32_t grantedModes() const { return ably_channel_granted_modes(raw_); }

    void setOptions(int rewind, uint32_t modes)
    {
        check(ably_channel_set_options(raw_, rewind, modes));
    }

    ErrorInfo lastError() const
    {
        return ErrorInfo::from_c(ably_channel_last_error(raw_));
    }

    void setOccupancyListener(ably_occupancy_cb_t cb, void *user_data = nullptr)
    {
        ably_channel_set_occupancy_listener(raw_, cb, user_data);
    }

    /* Fetch history via the realtime channel's associated REST client. */
    HistoryPage history(int limit = 0,
                         std::string_view direction = "backwards",
                         std::string_view from_serial = {})
    {
        ably_history_page_t *page = nullptr;
        check(ably_channel_history(raw_, limit,
                                    std::string(direction).c_str(),
                                    from_serial.empty() ? nullptr : std::string(from_serial).c_str(),
                                    &page));
        return HistoryPage(page);
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
        if (!opts.token.empty())     copts.token         = opts.token.c_str();

        raw_ = ably_rest_client_create(std::string(api_key).c_str(), &copts, nullptr);
        if (!raw_) throw Error(ABLY_ERR_NOMEM);
    }

    ~RestClient() { ably_rest_client_destroy(raw_); }

    RestClient(const RestClient &)            = delete;
    RestClient &operator=(const RestClient &) = delete;

    /* Single-message publish. */
    void publish(std::string_view channel, std::string_view name, std::string_view data)
    {
        check(ably_rest_publish(raw_,
            std::string(channel).c_str(),
            std::string(name).c_str(),
            std::string(data).c_str()));
    }

    /* Single-channel batch publish. */
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

    /* Multi-channel batch publish.
     * specs is a vector of {channel_name, messages} pairs. */
    void batchPublish(const std::vector<std::pair<std::string, std::vector<Message>>> &specs)
    {
        /* Build flat arrays of ably_rest_batch_spec_t. */
        std::vector<std::vector<ably_rest_message_t>> raw_msgs_all;
        std::vector<ably_rest_batch_spec_t>           raw_specs;
        raw_msgs_all.reserve(specs.size());
        raw_specs.reserve(specs.size());

        for (const auto &[ch, msgs] : specs) {
            raw_msgs_all.emplace_back();
            auto &rm = raw_msgs_all.back();
            rm.reserve(msgs.size());
            for (const auto &m : msgs) {
                ably_rest_message_t r{};
                r.name = m.name.c_str(); r.data = m.data.c_str();
                rm.push_back(r);
            }
            ably_rest_batch_spec_t spec{};
            spec.channel  = ch.c_str();
            spec.messages = rm.data();
            spec.count    = rm.size();
            raw_specs.push_back(spec);
        }

        std::vector<ably_rest_batch_result_t> results(specs.size() * 2);
        size_t result_count = 0;
        check(ably_rest_batch_publish(raw_,
                                       raw_specs.data(), raw_specs.size(),
                                       results.data(), results.size(),
                                       &result_count));
    }

    /* Channel history. */
    HistoryPage channelHistory(std::string_view channel,
                                int              limit     = 0,
                                std::string_view direction = "backwards",
                                std::string_view from_serial = {})
    {
        ably_history_page_t *page = nullptr;
        check(ably_rest_channel_history(raw_,
                                         std::string(channel).c_str(),
                                         limit,
                                         std::string(direction).c_str(),
                                         from_serial.empty() ? nullptr : std::string(from_serial).c_str(),
                                         &page));
        return HistoryPage(page);
    }

    /* Channel status. */
    ChannelStatus channelStatus(std::string_view channel)
    {
        ably_channel_status_t s{};
        check(ably_rest_channel_status(raw_, std::string(channel).c_str(), &s));
        return ChannelStatus::from_c(s);
    }

    /* Channel list. */
    ChannelListPage channelList(std::string_view prefix = {}, int limit = 0)
    {
        ably_channel_list_page_t *page = nullptr;
        check(ably_rest_channel_list(raw_,
                                      prefix.empty() ? nullptr : std::string(prefix).c_str(),
                                      limit,
                                      &page));
        return ChannelListPage(page);
    }

    /* REST presence.get(). */
    PresencePage presenceGet(std::string_view channel,
                              int              limit     = 0,
                              std::string_view client_id = {})
    {
        ably_presence_page_t *page = nullptr;
        check(ably_rest_presence_get(raw_,
                                      std::string(channel).c_str(),
                                      limit,
                                      client_id.empty() ? nullptr : std::string(client_id).c_str(),
                                      &page));
        return PresencePage(page);
    }

    /* Request a signed token. */
    TokenDetails requestToken(const TokenParams &params = {})
    {
        std::string cap = params.capability.empty() ? "{\"*\":[\"*\"]}" : params.capability;
        ably_token_params_t cp{};
        cp.capability = cap.c_str();
        cp.client_id  = params.client_id.empty() ? nullptr : params.client_id.c_str();
        cp.ttl_ms     = params.ttl_ms;

        ably_token_details_t d{};
        check(ably_rest_request_token(raw_, &cp, &d));
        return TokenDetails::from_c(d);
    }

    /* Generic REST request. */
    RestResponse request(std::string_view method,
                          std::string_view path,
                          std::string_view body = {})
    {
        ably_rest_response_t r{};
        check(ably_rest_request(raw_,
                                 std::string(method).c_str(),
                                 std::string(path).c_str(),
                                 body.empty() ? nullptr : std::string(body).c_str(),
                                 body.size(), &r));
        return RestResponse::from_c(r);
    }

    /* Server time. */
    int64_t time()
    {
        int64_t t = 0;
        check(ably_rest_time(raw_, &t));
        return t;
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
        if (!opts.client_id.empty())     copts.client_id = opts.client_id.c_str();

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
    std::string connectionId() const { return ably_rt_client_connection_id(raw_) ? ably_rt_client_connection_id(raw_) : ""; }
    std::string clientId()    const { return ably_rt_client_client_id(raw_)    ? ably_rt_client_client_id(raw_)    : ""; }
    ErrorInfo   lastError()   const { return ErrorInfo::from_c(ably_rt_client_last_error(raw_)); }

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
