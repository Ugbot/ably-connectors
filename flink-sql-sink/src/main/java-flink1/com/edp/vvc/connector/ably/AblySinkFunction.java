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
package com.edp.vvc.connector.ably;

import org.apache.flink.configuration.Configuration;
import org.apache.flink.streaming.api.functions.sink.RichSinkFunction;
import org.apache.flink.table.data.RowData;
import org.apache.flink.table.types.logical.LogicalType;
import org.apache.flink.types.RowKind;

import java.util.*;

/** Flink 1.x RichSinkFunction. Buffers rows and flushes via {@link AblyBatchPublisher}. */
public class AblySinkFunction extends RichSinkFunction<RowData> {

    private static final long serialVersionUID = 1L;

    private final String apiKey;
    private final String channelFieldName;
    private final int batchSize;
    private final String restEndpoint;
    private final String[] fieldNames;
    private final LogicalType[] fieldTypes;

    private transient AblyRowSerializer serializer;
    private transient AblyBatchPublisher publisher;
    private transient int channelFieldIndex;
    private transient List<RowData> buffer;

    AblySinkFunction(String apiKey, String channelFieldName, int batchSize,
                     String restEndpoint, String[] fieldNames, LogicalType[] fieldTypes) {
        this.apiKey = apiKey;
        this.channelFieldName = channelFieldName;
        this.batchSize = batchSize;
        this.restEndpoint = restEndpoint;
        this.fieldNames = fieldNames;
        this.fieldTypes = fieldTypes;
    }

    @Override
    public void open(Configuration parameters) {
        serializer = new AblyRowSerializer(fieldNames, fieldTypes);
        publisher = new AblyBatchPublisher(apiKey, restEndpoint);
        buffer = new ArrayList<>(batchSize);

        channelFieldIndex = -1;
        for (int i = 0; i < fieldNames.length; i++) {
            if (fieldNames[i].equals(channelFieldName)) { channelFieldIndex = i; break; }
        }
        if (channelFieldIndex < 0) {
            throw new IllegalArgumentException(
                    "channel.field '" + channelFieldName + "' not found in schema. " +
                    "Available columns: " + Arrays.toString(fieldNames));
        }
    }

    @Override
    public void invoke(RowData row, Context context) throws Exception {
        if (row.getRowKind() != RowKind.INSERT) return;
        buffer.add(row);
        if (buffer.size() >= batchSize) flush();
    }

    @Override
    public void close() throws Exception {
        if (buffer != null && !buffer.isEmpty()) flush();
    }

    private void flush() throws Exception {
        Map<String, List<Map<String, Object>>> byChannel = new LinkedHashMap<>();
        for (RowData row : buffer) {
            String channel = serializer.getChannelValue(row, channelFieldIndex);
            byChannel.computeIfAbsent(channel, k -> new ArrayList<>()).add(serializer.rowToMap(row));
        }
        buffer.clear();
        publisher.publish(byChannel);
    }
}
