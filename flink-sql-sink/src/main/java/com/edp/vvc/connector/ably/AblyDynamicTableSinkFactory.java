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

import org.apache.flink.configuration.ConfigOption;
import org.apache.flink.configuration.ConfigOptions;
import org.apache.flink.configuration.ReadableConfig;
import org.apache.flink.table.catalog.ResolvedSchema;
import org.apache.flink.table.connector.sink.DynamicTableSink;
import org.apache.flink.table.factories.DynamicTableSinkFactory;
import org.apache.flink.table.factories.FactoryUtil;
import org.apache.flink.table.types.logical.LogicalType;

import java.util.HashSet;
import java.util.Set;

/**
 * Registers the "ably" connector identifier with Flink's table factory SPI.
 * SQL usage:
 * <pre>
 *   CREATE TABLE my_sink (
 *     user_id  BIGINT,
 *     event    STRING,
 *     channel  STRING
 *   ) WITH (
 *     'connector'     = 'ably',
 *     'api-key'       = 'appId.keyId:secret',
 *     'channel.field' = 'channel',
 *     'batch.size'    = '100'
 *   );
 * </pre>
 */
public class AblyDynamicTableSinkFactory implements DynamicTableSinkFactory {

    public static final String IDENTIFIER = "ably";

    public static final ConfigOption<String> API_KEY = ConfigOptions
            .key("api-key")
            .stringType()
            .noDefaultValue()
            .withDescription("Ably API key in the format appId.keyId:secret. Used for Basic auth.");

    public static final ConfigOption<String> CHANNEL_FIELD = ConfigOptions
            .key("channel.field")
            .stringType()
            .noDefaultValue()
            .withDescription("Column name whose runtime value determines the target Ably channel.");

    public static final ConfigOption<Integer> BATCH_SIZE = ConfigOptions
            .key("batch.size")
            .intType()
            .defaultValue(100)
            .withDescription("Maximum rows buffered before a batch publish call is made. Also flushed on task close.");

    public static final ConfigOption<String> REST_ENDPOINT = ConfigOptions
            .key("rest.endpoint")
            .stringType()
            .defaultValue("https://main.realtime.ably.net")
            .withDescription("Ably REST API base URL. Override for testing or custom environments.");

    @Override
    public String factoryIdentifier() {
        return IDENTIFIER;
    }

    @Override
    public Set<ConfigOption<?>> requiredOptions() {
        Set<ConfigOption<?>> required = new HashSet<>();
        required.add(API_KEY);
        required.add(CHANNEL_FIELD);
        return required;
    }

    @Override
    public Set<ConfigOption<?>> optionalOptions() {
        Set<ConfigOption<?>> optional = new HashSet<>();
        optional.add(BATCH_SIZE);
        optional.add(REST_ENDPOINT);
        return optional;
    }

    @Override
    public DynamicTableSink createDynamicTableSink(Context context) {
        FactoryUtil.TableFactoryHelper helper = FactoryUtil.createTableFactoryHelper(this, context);
        helper.validate();

        ReadableConfig options = helper.getOptions();
        ResolvedSchema schema = context.getCatalogTable().getResolvedSchema();

        String[] fieldNames = schema.getColumnNames().toArray(new String[0]);
        LogicalType[] fieldTypes = schema.getColumnDataTypes().stream()
                .map(dt -> dt.getLogicalType())
                .toArray(LogicalType[]::new);

        return new AblyDynamicTableSink(
                options.get(API_KEY),
                options.get(CHANNEL_FIELD),
                options.get(BATCH_SIZE),
                options.get(REST_ENDPOINT),
                fieldNames,
                fieldTypes
        );
    }
}
