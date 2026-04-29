package com.edp.vvc.connector.ably;

import com.fasterxml.jackson.databind.ObjectMapper;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.util.*;

/**
 * Publishes row batches to the Ably REST batch endpoint.
 *
 * <p>Groups rows by channel and POSTs a single request containing one BatchSpec per channel:
 * <pre>
 * POST /messages
 * [
 *   { "channels": ["channel-a"], "messages": [{"data": "{...}"}, ...] },
 *   { "channels": ["channel-b"], "messages": [{"data": "{...}"}, ...] }
 * ]
 * </pre>
 *
 * Shared by both the Flink 1.x SinkFunction and the Flink 2.x SinkWriter.
 */
public class AblyBatchPublisher {

    private final String restEndpoint;
    private final String authHeader;
    private final ObjectMapper objectMapper;
    private final HttpClient httpClient;

    public AblyBatchPublisher(String apiKey, String restEndpoint) {
        this.restEndpoint = restEndpoint.replaceAll("/+$", "");
        String encoded = Base64.getEncoder().encodeToString(apiKey.getBytes(StandardCharsets.UTF_8));
        this.authHeader = "Basic " + encoded;
        this.objectMapper = new ObjectMapper();
        this.httpClient = HttpClient.newHttpClient();
    }

    /**
     * Publishes rows grouped by channel in a single batch request.
     *
     * @param rowsByChannel map of channel name → list of serialised row maps
     * @throws Exception on HTTP error or network failure
     */
    public void publish(Map<String, List<Map<String, Object>>> rowsByChannel) throws Exception {
        if (rowsByChannel.isEmpty()) return;

        List<Map<String, Object>> batchSpecs = new ArrayList<>(rowsByChannel.size());
        for (Map.Entry<String, List<Map<String, Object>>> entry : rowsByChannel.entrySet()) {
            List<Map<String, String>> messages = new ArrayList<>(entry.getValue().size());
            for (Map<String, Object> rowMap : entry.getValue()) {
                messages.add(Collections.singletonMap("data", objectMapper.writeValueAsString(rowMap)));
            }
            Map<String, Object> spec = new LinkedHashMap<>();
            spec.put("channels", Collections.singletonList(entry.getKey()));
            spec.put("messages", messages);
            batchSpecs.add(spec);
        }

        String body = objectMapper.writeValueAsString(batchSpecs);

        HttpRequest request = HttpRequest.newBuilder()
                .uri(URI.create(restEndpoint + "/messages"))
                .header("Authorization", authHeader)
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8))
                .build();

        HttpResponse<String> response = httpClient.send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() >= 400) {
            throw new RuntimeException(
                    "Ably batch publish failed: HTTP " + response.statusCode() + " — " + response.body());
        }
    }
}
