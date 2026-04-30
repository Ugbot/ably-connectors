![Version](https://img.shields.io/badge/version-1.0.0-blue)
![License](https://img.shields.io/badge/license-Apache%202.0-green)
![Flink](https://img.shields.io/badge/Flink-1.20%20%2F%202.x-orange)

# flink-sql-sink

A Flink SQL sink connector for publishing rows to [Ably](https://ably.com) channels using the batch REST API.

> **Not an official Ably library.**
> This is a personal project, not affiliated with, endorsed by, or supported by Ably.
> For production use, see the [official Ably SDK catalogue](https://ably.com/download).

---

## How it works

You declare an Ably channel as a Flink SQL sink table. A column in the table schema
determines which Ably channel each row is routed to at runtime. Rows are buffered
and published in batches via the [Ably batch REST API](https://ably.com/docs/api/rest-api#batch),
with a final flush on task close or checkpoint.

---

## Requirements

- Java 11+
- Maven 3.6+
- Apache Flink 1.20 (default) or 2.x
- An [Ably API key](https://ably.com/docs/getting-started/setup)

---

## Build

```sh
# Flink 1.x (default — Flink 1.20)
mvn package -DskipTests

# Flink 2.x
mvn package -DskipTests -Pflink2
```

The shaded JAR at `target/flink-connector-ably-1.0.0.jar` bundles all dependencies
(including Jackson) under a relocated package to avoid classpath conflicts.

---

## Flink SQL usage

Add the connector JAR to Flink's classpath or use `ADD JAR`, then:

```sql
CREATE TABLE ably_sink (
    user_id  BIGINT,
    event    STRING,
    payload  STRING,
    channel  STRING          -- routing column: each row's channel name
) WITH (
    'connector'     = 'ably',
    'api-key'       = 'appId.keyId:secret',
    'channel.field' = 'channel',
    'batch.size'    = '100'
);

INSERT INTO ably_sink
SELECT user_id, event_type, to_json(MAP['value', CAST(amount AS STRING)]), channel_name
FROM my_source_table;
```

---

## Configuration options

| Option | Required | Default | Description |
|---|---|---|---|
| `connector` | Yes | — | Must be `'ably'` |
| `api-key` | Yes | — | Ably API key: `appId.keyId:secret` |
| `channel.field` | Yes | — | Column name whose runtime value is the target channel |
| `batch.size` | No | `100` | Max rows buffered per batch publish call |
| `rest.endpoint` | No | `https://main.realtime.ably.net` | Ably REST base URL; override for testing |

Row data is serialised to JSON using the Ably batch message format. Each row becomes
one message; the `channel.field` column value is used as the channel name and is not
included in the message payload.

---

## Architecture

`AblyDynamicTableSinkFactory` registers the `ably` connector identifier with Flink's
table factory SPI. At runtime it creates an `AblyDynamicTableSink`, which in turn
creates an `AblySinkFunction` (Flink 1.x) or `AblySinkWriter` (Flink 2.x) per
parallel subtask. Each function/writer buffers rows in `AblyBatchPublisher` and
flushes them to the Ably REST batch endpoint when the buffer reaches `batch.size`
or when the task closes.

---

## Known limitations

- **Append-only** — CDC operations (UPDATE_BEFORE / UPDATE_AFTER / DELETE) are not supported
- **No per-row encoding override** — all rows are published as JSON strings
- **Fixed batch size** — there is no time-based flush trigger; flush occurs at `batch.size` rows or task close
- **No delivery guarantees beyond at-least-once** — on task restart after failure, buffered rows that were not yet flushed may be re-sent

---

## License

Apache 2.0 — see [LICENSE](../LICENSE).
