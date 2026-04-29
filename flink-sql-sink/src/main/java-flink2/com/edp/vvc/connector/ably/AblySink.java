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

import org.apache.flink.api.connector.sink2.Sink;
import org.apache.flink.api.connector.sink2.SinkWriter;
import org.apache.flink.api.connector.sink2.WriterInitContext;
import org.apache.flink.table.data.RowData;
import org.apache.flink.table.types.logical.LogicalType;

import java.io.IOException;

/** Flink 2.x Sink<RowData> — entry point for the new unified Sink API. */
public class AblySink implements Sink<RowData> {

    private static final long serialVersionUID = 1L;

    private final String apiKey;
    private final String channelField;
    private final int batchSize;
    private final String restEndpoint;
    private final String[] fieldNames;
    private final LogicalType[] fieldTypes;

    AblySink(String apiKey, String channelField, int batchSize,
             String restEndpoint, String[] fieldNames, LogicalType[] fieldTypes) {
        this.apiKey = apiKey;
        this.channelField = channelField;
        this.batchSize = batchSize;
        this.restEndpoint = restEndpoint;
        this.fieldNames = fieldNames;
        this.fieldTypes = fieldTypes;
    }

    @Override
    public SinkWriter<RowData> createWriter(WriterInitContext context) throws IOException {
        return new AblySinkWriter(apiKey, channelField, batchSize, restEndpoint, fieldNames, fieldTypes);
    }
}
