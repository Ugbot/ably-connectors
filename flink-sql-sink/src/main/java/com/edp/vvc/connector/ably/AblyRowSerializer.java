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

import org.apache.flink.table.data.DecimalData;
import org.apache.flink.table.data.RowData;
import org.apache.flink.table.data.TimestampData;
import org.apache.flink.table.types.logical.DecimalType;
import org.apache.flink.table.types.logical.LocalZonedTimestampType;
import org.apache.flink.table.types.logical.LogicalType;
import org.apache.flink.table.types.logical.TimestampType;

import java.time.LocalDate;
import java.util.*;

/**
 * Converts a Flink {@link RowData} to a {@code Map<String, Object>} suitable for JSON
 * serialisation. Handles all common SQL types; unknown types emit a descriptive placeholder.
 *
 * Shared by both the Flink 1.x SinkFunction and the Flink 2.x SinkWriter.
 */
public class AblyRowSerializer {

    private final String[] fieldNames;
    private final LogicalType[] fieldTypes;

    public AblyRowSerializer(String[] fieldNames, LogicalType[] fieldTypes) {
        this.fieldNames = fieldNames;
        this.fieldTypes = fieldTypes;
    }

    /**
     * Returns the value of the channel routing field as a String.
     * Falls back to {@code "default"} when the field is null.
     */
    public String getChannelValue(RowData row, int channelFieldIndex) {
        if (row.isNullAt(channelFieldIndex)) return "default";
        return row.getString(channelFieldIndex).toString();
    }

    /** Converts all fields of the row to a map, preserving insertion order. */
    public Map<String, Object> rowToMap(RowData row) {
        Map<String, Object> map = new LinkedHashMap<>(fieldNames.length);
        for (int i = 0; i < fieldNames.length; i++) {
            map.put(fieldNames[i], row.isNullAt(i) ? null : extractField(row, i, fieldTypes[i]));
        }
        return map;
    }

    private Object extractField(RowData row, int pos, LogicalType type) {
        switch (type.getTypeRoot()) {
            case CHAR:
            case VARCHAR:
                return row.getString(pos).toString();

            case BOOLEAN:
                return row.getBoolean(pos);

            case TINYINT:
                return row.getByte(pos);

            case SMALLINT:
                return row.getShort(pos);

            case INTEGER:
            case INTERVAL_YEAR_MONTH:
                return row.getInt(pos);

            case BIGINT:
            case INTERVAL_DAY_TIME:
                return row.getLong(pos);

            case FLOAT:
                return row.getFloat(pos);

            case DOUBLE:
                return row.getDouble(pos);

            case DECIMAL: {
                DecimalType dt = (DecimalType) type;
                DecimalData dec = row.getDecimal(pos, dt.getPrecision(), dt.getScale());
                return dec != null ? dec.toBigDecimal() : null;
            }

            case DATE:
                return LocalDate.ofEpochDay(row.getInt(pos)).toString();

            case TIME_WITHOUT_TIME_ZONE: {
                int totalMillis = row.getInt(pos);
                int ms = totalMillis % 1000;
                int totalSecs = totalMillis / 1000;
                int sec = totalSecs % 60;
                int min = (totalSecs / 60) % 60;
                int hour = totalSecs / 3600;
                return String.format("%02d:%02d:%02d.%03d", hour, min, sec, ms);
            }

            case TIMESTAMP_WITHOUT_TIME_ZONE: {
                TimestampType tt = (TimestampType) type;
                TimestampData ts = row.getTimestamp(pos, tt.getPrecision());
                return ts != null ? ts.toLocalDateTime().toString() : null;
            }

            case TIMESTAMP_WITH_LOCAL_TIME_ZONE: {
                LocalZonedTimestampType lztt = (LocalZonedTimestampType) type;
                TimestampData ts = row.getTimestamp(pos, lztt.getPrecision());
                return ts != null ? ts.toInstant().toString() : null;
            }

            case BINARY:
            case VARBINARY:
                return Base64.getEncoder().encodeToString(row.getBinary(pos));

            default:
                return "<" + type.getTypeRoot().name().toLowerCase() + ">";
        }
    }
}
