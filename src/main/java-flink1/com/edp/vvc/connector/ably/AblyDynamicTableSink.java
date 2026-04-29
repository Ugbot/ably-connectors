package com.edp.vvc.connector.ably;

import org.apache.flink.table.connector.ChangelogMode;
import org.apache.flink.table.connector.sink.DynamicTableSink;
import org.apache.flink.table.connector.sink.SinkFunctionProvider;
import org.apache.flink.table.types.logical.LogicalType;

import java.util.Arrays;

/** Flink 1.x implementation — uses the (deprecated but functional) SinkFunctionProvider. */
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

    @SuppressWarnings("deprecation")
    @Override
    public SinkRuntimeProvider getSinkRuntimeProvider(Context context) {
        return SinkFunctionProvider.of(new AblySinkFunction(
                apiKey, channelField, batchSize, restEndpoint, fieldNames, fieldTypes));
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
