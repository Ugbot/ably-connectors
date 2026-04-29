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
