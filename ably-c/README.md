![Version](https://img.shields.io/badge/version-0.1.0-blue)
![License](https://img.shields.io/badge/license-Apache%202.0-green)

# ably-c

A C11 client library for building realtime experiences with [Ably](https://ably.com).

> **Not an official Ably library.**
> This is a personal project, not affiliated with, endorsed by, or supported by Ably.
> For production use, see the [official Ably SDK catalogue](https://ably.com/download).

---

## Features

- **REST publishing** — HTTP/1.1 over TLS, single and batch message publish
- **Realtime pub/sub** — WebSocket with automatic reconnection and exponential backoff
- **Session resume** — reconnects send `?resume=<key>` to recover missed messages
- **Channel subscriptions** — name-filtered or catch-all, multiple subscribers per channel
- **VCDIFF delta compression** — opt-in per channel; subscribers always receive full decoded payloads
- **Wire encoding** — JSON (default) or MessagePack, selected per client
- **TigerStyle** — ring buffers, no hot-path allocation, fixed-capacity pre-allocated structures
- **Custom allocator** — swap in any arena/pool allocator via `ably_allocator_t`
- **C++17 binding** — header-only RAII wrappers in `<ably/ably.hpp>`
- **Vendored** — zero external dependencies; everything builds from source

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

Tests: `allocator`, `base64`, `delta`, `protocol_json`, `protocol_msgpack`

### Integration tests (requires `ABLY_API_KEY`)

```sh
ABLY_API_KEY=appId.keyId:secret ctest -L integration --output-on-failure -V
```

Tests: `rest_integration`, `realtime_integration`, `e2e_pubsub`, `e2e_delta`

### Sanitizer build

```sh
cmake -B build-asan -DABLY_SANITIZE=ON
cmake --build build-asan
ABLY_API_KEY=... ctest --test-dir build-asan --output-on-failure
```

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

## Capacity constants

Override at compile time with `-D<NAME>=<VALUE>`:

| Constant | Default | Meaning |
|---|---|---|
| `ABLY_MAX_CHANNELS` | 64 | Channels per realtime client |
| `ABLY_MAX_SUBSCRIBERS_PER_CHANNEL` | 32 | Subscribers per channel |
| `ABLY_SEND_RING_CAPACITY` | 256 | Outbound frame ring buffer (must be power of two) |
| `ABLY_MAX_CHANNEL_NAME_LEN` | 256 | Max channel name length (bytes) |
| `ABLY_MAX_MESSAGE_NAME_LEN` | 256 | Max message name length (bytes) |
| `ABLY_MAX_MESSAGE_DATA_LEN` | 32768 | Max message data length (bytes) |

---

## Vendored libraries

| Library | Version | License | Purpose |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.4 | Apache 2.0 | TLS 1.2/1.3, TCP sockets, CTRNG |
| [wslay](https://github.com/tatsuhiro-t/wslay) | 1.1.1 | MIT | WebSocket RFC 6455 framing |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | JSON encode/decode |
| [mpack](https://github.com/ludocode/mpack) | 1.1.1 | MIT | MessagePack encode/decode |

---

## Known limitations

- **No presence** — presence subscribe/enter/leave are not implemented
- **No auth token refresh** — only Basic auth (API key) is supported; token auth is not
- **No push notifications** — the push notification API is not implemented
- **Session resume** — the `?resume=` parameter is sent on reconnect, but gap recovery
  (requesting missed messages) is not yet handled
- **32 KB message data cap** — `ABLY_MAX_MESSAGE_DATA_LEN` defaults to 32768 bytes;
  raise it at compile time for larger payloads

---

## License

Apache 2.0 — see [LICENSE](LICENSE). Vendored components carry their own licenses
(all MIT or Apache 2.0) as noted in each `vendor/` directory.
