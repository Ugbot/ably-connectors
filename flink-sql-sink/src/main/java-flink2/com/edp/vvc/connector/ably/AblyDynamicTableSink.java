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

import org.apache.flink.table.connector.ChangelogMode;
import org.apache.flink.table.connector.sink.DataStreamSinkProvider;
import org.apache.flink.table.connector.sink.DynamicTableSink;
import org.apache.flink.table.types.logical.LogicalType;

import java.util.Arrays;

/** Flink 2.x implementation — uses the new SinkProvider + Sink<RowData> API. */
public class AblyDynamicTableSink implements DynamicTableSink {

    private final String apiKey;
    private final String channelField;
    private final int batchSize;
    private final String restEndpoint;
    private final String[] fieldNames;
    private final LogicalType[] fieldTypes;

    AblyDynamicTableSink(String apiKey, String channelField, int batchSize,
                         String restEndpoint, String[] fieldNames, LogicalType[] fieldTypes) {
        this.apiKey = apiKey;
        this.channelField = channelField;
        this.batchSize = batchSize;
        this.restEndpoint = restEndpoint;
        this.fieldNames = fieldNames;
        this.fieldTypes = fieldTypes;
    }

    @Override
    public ChangelogMode getChangelogMode(ChangelogMode requestedMode) {
        return ChangelogMode.insertOnly();
    }

    @Override
    public SinkRuntimeProvider getSinkRuntimeProvider(Context context) {
        AblySink sink = new AblySink(apiKey, channelField, batchSize, restEndpoint, fieldNames, fieldTypes);
        return (DataStreamSinkProvider) (providerContext, dataStream) -> dataStream.sinkTo(sink);
    }

    @Override
    public DynamicTableSink copy() {
        return new AblyDynamicTableSink(apiKey, channelField, batchSize, restEndpoint,
                Arrays.copyOf(fieldNames, fieldNames.length),
                Arrays.copyOf(fieldTypes, fieldTypes.length));
    }

    @Override
    public String asSummaryString() {
        return "Ably Sink [channel.field=" + channelField + ", batch.size=" + batchSize + "]";
    }
}
