# ably-connectors

A personal collection of [Ably](https://ably.com) connectors — integrations, client libraries, and sink adapters built and used over the years. Published here for others to read, use, and comment on.

> **Note:** These are personal projects, not official Ably libraries.
> They are not affiliated with, endorsed by, or supported by Ably.

---

## Connectors

### [`flink-sql-sink/`](./flink-sql-sink/)

**Apache Flink SQL Sink Connector for Ably**

A Flink SQL connector that lets you declare an Ably channel as a sink table in Flink SQL. Rows are batched and published to Ably via the REST batch API. Supports both Flink 1.x and Flink 2.x through dual build profiles.

- Language: Java 11
- Build: Maven (`mvn package -DskipTests`)
- Flink versions: 1.20, 2.0

### [`ably-c/`](./ably-c/)

**C Client Library for Ably**

A self-contained C11 library with zero external dependencies. Everything is vendored — TLS (mbedTLS), WebSocket framing (wslay), JSON (cJSON), and MessagePack (mpack). Provides full real-time publish/subscribe over WebSocket and REST publishing over HTTPS. An optional C++17 header-only binding is included.

- Language: C11 (+ optional C++17 header)
- Build: CMake 3.16+
- Platforms: Linux, macOS, Windows; Android and iOS supported
- Design: TigerStyle — ring buffers, no hot-path allocation, pre-allocated fixed-capacity structures

---

## License

All original code in this repository is licensed under the [Apache License 2.0](./LICENSE).

Vendored third-party libraries retain their own licenses (MIT or Apache 2.0) as noted in each `vendor/` directory.
