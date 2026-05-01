![Version](https://img.shields.io/badge/version-0.2.0-blue)
![License](https://img.shields.io/badge/license-Apache%202.0-green)

# ably-c

A C11 client library for building realtime experiences with [Ably](https://ably.com).

> **Not an official Ably library.**
> This is a personal project, not affiliated with, endorsed by, or supported by Ably.
> For production use, see the [official Ably SDK catalogue](https://ably.com/download).

---

## Features

**Realtime (WebSocket)**
- Pub/sub with name-filtered or catch-all channel subscriptions
- Automatic reconnection with exponential backoff and full-jitter
- Fallback host cycling (`a–e.ably-realtime.com`) on primary host failure
- Session resume — sends `?resume=<key>&connectionSerial=<n>` on reconnect
- Gap recovery — backfills missed messages from history on non-resumed ATTACHED
- Channel rewind — replay the last N messages on attach
- VCDIFF delta compression — opt-in per channel; subscribers receive full payloads
- Presence — enter/leave/update, subscribe to events, get current members
- Occupancy metrics — live subscriber/publisher counts via `params.occupancy`
- Channel modes — bitmask control of PUBLISH/SUBSCRIBE/PRESENCE capabilities
- Token auth callback — renew expired tokens automatically on 401 disconnect
- CONNECTED frame parsing — applies server-provided clientId, connectionStateTtl

**REST**
- Single-message publish (idempotent with explicit ID)
- Single-channel batch publish
- Multi-channel batch publish (`POST /messages`)
- Channel history with pagination (forwards / backwards)
- Channel status and occupancy
- REST channel list with prefix filter and pagination
- REST presence.get with clientId filter and pagination
- Ably stats with pagination
- Token request (`requestToken`) — HMAC-SHA256 signed, `POST /keys/{keyName}/requestToken`
- Bearer token auth — pass a pre-obtained token via `opts.token`
- Generic request (`ably_rest_request`) for arbitrary Ably REST calls
- Server time

**Design**
- TigerStyle — ring buffers, fixed-capacity pre-allocated structures, no hot-path allocation
- Custom allocator — swap in any arena/pool via `ably_allocator_t`
- Wire encoding — JSON (default) or MessagePack, selected per client
- C++17 header-only binding — RAII wrappers in `<ably/ably.hpp>`
- Bring-your-own event loop — `ably_rt_step()` + `ably_rt_client_fd()`
- Vendored — zero external dependencies; everything builds from source

---

## Supported platforms

| Platform | Architecture | Notes |
|---|---|---|
| Linux | x86-64, arm64 | Tested on Ubuntu 22.04 |
| macOS | x86-64, Apple Silicon | Tested on macOS 15 |
| Windows | x86-64 | MSVC 2019+ or MinGW |
| Android | arm64-v8a, armeabi-v7a | CMake NDK toolchain |
| iOS | arm64 | CMake iOS toolchain |

---

## Build

All dependencies are vendored — **no system packages required**.

```sh
git clone <this-repo>
cd ably-connectors/ably-c
cmake -B build
cmake --build build
```

### Install (optional)

```sh
cmake --install build --prefix /usr/local
```

This copies `libably.a` and the public headers to `include/ably/`.

---

## Usage

### REST publish (C)

```c
#include <ably/ably.h>

ably_rest_client_t *client = ably_rest_client_create("appId.keyId:secret", NULL, NULL);

ably_error_t err = ably_rest_publish(client, "my-channel", "greeting", "Hello!");
if (err != ABLY_OK) {
    fprintf(stderr, "publish failed: %s\n", ably_error_str(err));
}

ably_rest_client_destroy(client);
```

### Realtime subscribe (C)

```c
#include <ably/ably.h>

static void on_message(ably_channel_t *ch, const ably_message_t *msg, void *ud)
{
    printf("[%s] %s: %s\n", ably_channel_name(ch), msg->name, msg->data);
}

ably_rt_client_t *client = ably_rt_client_create("appId.keyId:secret", NULL, NULL);
ably_rt_client_connect(client);

/* Spin until connected ... */
while (ably_rt_client_state(client) != ABLY_CONN_CONNECTED) { /* sleep */ }

ably_channel_t *ch = ably_rt_channel_get(client, "my-channel");
ably_channel_subscribe(ch, NULL, on_message, NULL);
ably_channel_attach(ch);

/* Run event loop ... */

ably_rt_client_close(client, 5000);
ably_rt_client_destroy(client);
```

### C++ API

```cpp
#include <ably/ably.hpp>
#include <iostream>

int main()
{
    ably::RealtimeClient client("appId.keyId:secret");
    client.connect();

    while (client.state() != ABLY_CONN_CONNECTED) { /* sleep */ }

    auto &ch = client.channel("my-channel");
    ch.subscribe(std::nullopt, [](const ably::Message &m) {
        std::cout << m.name << ": " << m.data << "\n";
    });
    ch.attach();

    /* ... */

    client.close();
}
```

---

## Token authentication

### Request a signed token (REST)

```c
ably_token_params_t params = {
    .capability = "{\"my-channel\":[\"publish\",\"subscribe\"]}",
    .ttl_ms     = 3600000,  /* 1 hour */
};
ably_token_details_t token;
ably_error_t err = ably_rest_request_token(client, &params, &token);
printf("token: %s (expires %" PRId64 ")\n", token.token, token.expires);
```

### Bearer token auth

```c
ably_rest_options_t opts;
ably_rest_options_init(&opts);
opts.token = my_previously_obtained_token;

ably_rest_client_t *client = ably_rest_client_create("appId.keyId:secret", &opts, NULL);
```

### Token renewal callback (realtime)

```c
static ably_error_t refresh_token(ably_rt_client_t *client,
                                   char *token_out, size_t token_out_len,
                                   void *user_data)
{
    /* Fetch a fresh token from your server, write to token_out. */
    snprintf(token_out, token_out_len, "%s", fetch_token_from_server());
    return ABLY_OK;
}

ably_rt_options_t opts;
ably_rt_options_init(&opts);
opts.auth_cb        = refresh_token;
opts.auth_user_data = NULL;

ably_rt_client_t *client = ably_rt_client_create("appId.keyId:secret", &opts, NULL);
```

---

## Presence

```c
static void on_presence(ably_channel_t *ch,
                         const ably_presence_message_t *msg, void *ud)
{
    const char *action = msg->action == ABLY_PRESENCE_ENTER ? "ENTER" : "LEAVE";
    printf("%s: %s\n", action, msg->client_id);
}

ably_channel_t *ch = ably_rt_channel_get(client, "chat");
ably_channel_presence_subscribe(ch, on_presence, NULL);
ably_channel_attach(ch);

ably_channel_presence_enter(ch, "alice", "{\"status\":\"online\"}");

/* Later: */
ably_presence_message_t members[32];
int total = 0;
int written = ably_channel_presence_get_members(ch, members, 32, &total);
```

---

## Channel history

```c
ably_history_page_t *page = NULL;
ably_error_t err = ably_rest_channel_history(rest_client, "my-channel",
                                               100, "backwards", NULL, &page);
if (err == ABLY_OK && page) {
    for (size_t i = 0; i < page->count; i++)
        printf("%s: %s\n", page->items[i].name, page->items[i].data);

    /* Paginate: */
    if (page->next_cursor[0]) {
        ably_history_page_t *page2 = NULL;
        ably_rest_channel_history(rest_client, "my-channel",
                                   100, "backwards", page->next_cursor, &page2);
        ably_history_page_free(page2);
    }
    ably_history_page_free(page);
}

/* Via realtime channel (creates temp REST client internally): */
err = ably_channel_history(rt_channel, 10, "backwards", NULL, &page);
```

---

## Delta compression

Enable per-channel VCDIFF delta compression before attaching. The client decodes
deltas transparently — subscribers always receive the full string payload.

```c
ably_channel_t *ch = ably_rt_channel_get(client, "high-frequency-channel");
ably_channel_enable_delta(ch);   /* must be called before attach */
ably_channel_subscribe(ch, NULL, on_message, NULL);
ably_channel_attach(ch);
```

---

## Channel rewind

Replay the last N messages immediately on attach.

```c
ably_channel_set_rewind(ch, 5);   /* replay last 5 messages */
ably_channel_attach(ch);
```

---

## Channel modes

Request specific capabilities in the ATTACH frame.

```c
ably_channel_set_modes(ch,
    ABLY_CHANNEL_MODE_SUBSCRIBE | ABLY_CHANNEL_MODE_PRESENCE_SUBSCRIBE);
ably_channel_attach(ch);

/* After ATTACHED: */
uint32_t granted = ably_channel_granted_modes(ch);
```

---

## Bring-your-own event loop

```c
/* 1. Perform TLS + WebSocket handshake (blocking). */
ably_rt_client_connect_async(client);

/* 2. Register ably_rt_client_fd() with your event loop for readable events. */
int fd = ably_rt_client_fd(client);

/* 3. On readable (or periodically): */
int result = ably_rt_step(client, 0 /* timeout_ms: 0 = non-blocking */);
/* result: 1 = work done, 0 = idle, -1 = error/disconnected */
```

---

## Error inspection

```c
/* Last connection-level error: */
const ably_error_info_t *err = ably_rt_client_last_error(client);
printf("Ably code %d: %s\n", err->ably_code, err->message);

/* Last channel-level error: */
const ably_error_info_t *cerr = ably_channel_last_error(channel);
```

---

## CMake options

| Option | Default | Description |
|---|---|---|
| `ABLY_BUILD_TESTS` | ON | Build test programs |
| `ABLY_BUILD_EXAMPLES` | ON | Build example programs |
| `ABLY_BUILD_CPP` | ON | Compile-check the C++17 binding |
| `ABLY_ENCODING_JSON` | ON | Include JSON codec |
| `ABLY_ENCODING_MSGPACK` | ON | Include MessagePack codec |
| `ABLY_SANITIZE` | OFF | Enable ASan + UBSan |

---

## Running tests

### Unit tests (no network)

```sh
cd build && ctest -L unit --output-on-failure
```

Tests: `hashmap`, `allocator`, `base64`, `delta`, `protocol_json`, `protocol_msgpack`, `presence_proto`

### Integration tests (requires `ABLY_API_KEY`)

```sh
ABLY_API_KEY=appId.keyId:secret ctest -L integration --output-on-failure -V
```

Tests: `rest_integration`, `realtime_integration`, `history_integration`,
`e2e_pubsub`, `e2e_delta`, `e2e_presence`

### Sanitizer build

```sh
cmake -B build-asan -DABLY_SANITIZE=ON
cmake --build build-asan
ABLY_API_KEY=... ctest --test-dir build-asan --output-on-failure
```

---

## Capacity constants

Override at compile time with `-D<NAME>=<VALUE>`:

| Constant | Default | Meaning |
|---|---|---|
| `ABLY_MAX_CHANNELS` | 64 | Channels per realtime client |
| `ABLY_MAX_SUBSCRIBERS_PER_CHANNEL` | 32 | Subscribers per channel |
| `ABLY_MAX_PRESENCE_MEMBERS` | 128 | Presence members per channel |
| `ABLY_SEND_RING_CAPACITY` | 256 | Outbound frame ring buffer (must be power of two) |
| `ABLY_MAX_CHANNEL_NAME_LEN` | 256 | Max channel name length (bytes) |
| `ABLY_MAX_MESSAGE_NAME_LEN` | 256 | Max message name length (bytes) |
| `ABLY_MAX_MESSAGE_DATA_LEN` | 32768 | Max message data length (bytes) |
| `ABLY_MAX_CLIENT_ID_LEN` | 256 | Max clientId length (bytes) |

---

## Custom allocator

```c
static void *my_malloc (size_t size,            void *ud) { return arena_alloc(ud, size); }
static void  my_free   (void *ptr,              void *ud) { arena_free(ud, ptr); }
static void *my_realloc(void *ptr, size_t size, void *ud) { return arena_realloc(ud, ptr, size); }

ably_allocator_t alloc = {
    .malloc_fn  = my_malloc,
    .free_fn    = my_free,
    .realloc_fn = my_realloc,
    .user_data  = &my_arena,
};

ably_rt_client_t *client = ably_rt_client_create(api_key, NULL, &alloc);
```

---

## Known limitations

- **No push notifications** — the push notification API is not implemented
- **No AES message encryption** — cipher/encrypt options are not implemented
- **No JWT auth** — only API key and Ably token auth are supported
- **32 KB message data cap** — `ABLY_MAX_MESSAGE_DATA_LEN` defaults to 32768 bytes;
  raise it at compile time for larger payloads

---

## Vendored libraries

| Library | Version | License | Purpose |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.4 | Apache 2.0 | TLS 1.2/1.3, TCP sockets, CTRNG, HMAC-SHA256 |
| [wslay](https://github.com/tatsuhiro-t/wslay) | 1.1.1 | MIT | WebSocket RFC 6455 framing |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | JSON encode/decode |
| [mpack](https://github.com/ludocode/mpack) | 1.1.1 | MIT | MessagePack encode/decode |

---

## License

Apache 2.0 — see [LICENSE](LICENSE). Vendored components carry their own licenses
(all MIT or Apache 2.0) as noted in each `vendor/` directory.
