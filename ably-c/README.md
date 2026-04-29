# ably-c

A self-contained C11 client library for [Ably](https://ably.com) real-time messaging and REST publishing.

All dependencies are vendored — **no system packages required**.  Builds on Linux, macOS, and Windows with a single CMake invocation.

---

## Features

- **REST publishing** — HTTP/1.1 over TLS with Basic auth, single and batch message publish
- **Real-time pub/sub** — WebSocket with automatic reconnection and exponential backoff
- **Channel subscriptions** — name-filtered or catch-all, multiple subscribers per channel
- **Wire encoding** — JSON (default) or MessagePack, selected per client
- **TigerStyle** — no allocation on any hot path; fixed-capacity ring buffers; all memory claimed at create time
- **Custom allocator** — swap in any arena/pool allocator via `ably_allocator_t`
- **C++17 binding** — header-only RAII wrappers in `<ably/ably.hpp>`
- **Platforms** — Linux, macOS, Windows; Android and iOS (bonus, via CMake toolchains)

---

## Vendored libraries

| Library | Version | License | Purpose |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.4 | Apache 2.0 | TLS 1.2/1.3, TCP sockets, CTRNG |
| [wslay](https://github.com/tatsuhiro-t/wslay) | 1.1.1 | MIT | WebSocket RFC 6455 framing |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | JSON encode/decode |
| [mpack](https://github.com/ludocode/mpack) | 1.1.1 | MIT | MessagePack encode/decode |

---

## Quick start

### Build

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

This installs `libably.a` and the public headers under `include/ably/`.

---

## C API

### REST publish

```c
#include <ably/ably.h>

ably_rest_client_t *client = ably_rest_client_create("appId.keyId:secret", NULL, NULL);

ably_error_t err = ably_rest_publish(client, "my-channel", "greeting", "Hello!");
if (err != ABLY_OK) {
    fprintf(stderr, "publish failed: %s\n", ably_error_str(err));
}

ably_rest_client_destroy(client);
```

### Real-time subscribe

```c
#include <ably/ably.h>

static void on_message(ably_channel_t *ch, const ably_message_t *msg, void *ud)
{
    printf("[%s] %s: %s\n", ably_channel_name(ch), msg->name, msg->data);
}

ably_rt_client_t *client = ably_rt_client_create("appId.keyId:secret", NULL, NULL);
ably_rt_client_connect(client);

/* Wait for CONNECTED ... */

ably_channel_t *channel = ably_rt_channel_get(client, "my-channel");
ably_channel_subscribe(channel, NULL, on_message, NULL);
ably_channel_attach(channel);

/* Event loop ... */

ably_rt_client_close(client, 5000);
ably_rt_client_destroy(client);
```

---

## C++ API

```cpp
#include <ably/ably.hpp>
#include <iostream>

int main()
{
    ably::RealtimeClient client("appId.keyId:secret");

    client.connect();
    /* wait for ABLY_CONN_CONNECTED ... */

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

Tests: `allocator`, `base64`, `protocol_json`, `protocol_msgpack`

### Integration tests (requires `ABLY_API_KEY`)

```sh
ABLY_API_KEY=appId.keyId:secret ctest -L integration --output-on-failure
```

Tests: `rest` (REST publish + HTTP status), `realtime` (connect, attach, subscribe, roundtrip message)

### Sanitizer build

```sh
cmake -B build-asan -DABLY_SANITIZE=ON
cmake --build build-asan
ABLY_API_KEY=... cd build-asan && ctest --output-on-failure
```

---

## Custom allocator

```c
static void *my_malloc(size_t size, void *ud)  { return arena_alloc(ud, size); }
static void  my_free  (void *ptr,  void *ud)  { arena_free(ud, ptr); }
static void *my_realloc(void *ptr, size_t sz, void *ud) { return arena_realloc(ud, ptr, sz); }

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
| `ABLY_MAX_CHANNELS` | 64 | Channels per real-time client |
| `ABLY_MAX_SUBSCRIBERS_PER_CHANNEL` | 32 | Subscribers per channel |
| `ABLY_SEND_RING_CAPACITY` | 256 | Outbound frame ring buffer (must be power of two) |
| `ABLY_MAX_CHANNEL_NAME_LEN` | 256 | Max channel name length (bytes) |
| `ABLY_MAX_MESSAGE_NAME_LEN` | 256 | Max message name length (bytes) |
| `ABLY_MAX_MESSAGE_DATA_LEN` | 32768 | Max message data length (bytes) |

---

## License

Apache 2.0 — see [LICENSE](LICENSE).  Vendored components carry their own licenses (all MIT or Apache 2.0).
