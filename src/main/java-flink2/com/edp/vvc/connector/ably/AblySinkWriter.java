package com.edp.vvc.connector.ably;

import org.apache.flink.api.connector.sink2.SinkWriter;
import org.apache.flink.table.data.RowData;
import org.apache.flink.table.types.logical.LogicalType;
import org.apache.flink.types.RowKind;

import java.io.IOException;
import java.util.*;

/** Flink 2.x SinkWriter. Buffers rows and flushes via {@link AblyBatchPublisher}. */
public class AblySinkWriter implements SinkWriter<RowData> {

    private final AblyRowSerializer serializer;
    private final AblyBatchPublisher publisher;
    private final int batchSize;
    private final int channelFieldIndex;
    private final List<RowData> buffer;

    AblySinkWriter(String apiKey, String channelFieldName, int batchSize,
                   String restEndpoint, String[] fieldNames, LogicalType[] fieldTypes) {
        this.serializer = new AblyRowSerializer(fieldNames, fieldTypes);
        this.publisher = new AblyBatchPublisher(apiKey, restEndpoint);
        this.batchSize = batchSize;
        this.buffer = new ArrayList<>(batchSize);

        int idx = -1;
        for (int i = 0; i < fieldNames.length; i++) {
            if (fieldNames[i].equals(channelFieldName)) { idx = i; break; }
        }
        if (idx < 0) {
            throw new IllegalArgumentException(
                    "channel.field '" + channelFieldName + "' not found in schema. " +
                    "Available columns: " + Arrays.toString(fieldNames));
        }
        this.channelFieldIndex = idx;
    }

    @Override
    public void write(RowData element, Context context) throws IOException, InterruptedException {
        if (element.getRowKind() != RowKind.INSERT) return;
        buffer.add(element);
        if (buffer.size() >= batchSize) flush(false);
    }

    @Override
    public void flush(boolean endOfInput) throws IOException, InterruptedException {
        if (buffer.isEmpty()) return;
        try {
            Map<String, List<Map<String, Object>>> byChannel = new LinkedHashMap<>();
            for (RowData row : buffer) {
                String channel = serializer.getChannelValue(row, channelFieldIndex);
                byChannel.computeIfAbsent(channel, k -> new ArrayList<>()).add(serializer.rowToMap(row));
            }
            buffer.clear();
            publisher.publish(byChannel);
        } catch (Exception e) {
            throw new IOException("Ably flush failed", e);
        }
    }

    @Override
    public void close() throws Exception {
        flush(true);
    }
}
