cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Unit tests (no network required)
# ---------------------------------------------------------------------------

add_executable(test_allocator test_allocator.c ../src/alloc.c)
target_include_directories(test_allocator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME allocator COMMAND test_allocator)
set_tests_properties(allocator PROPERTIES LABELS "unit")

add_executable(test_base64 test_base64.c ../src/base64.c)
target_include_directories(test_base64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
add_test(NAME base64 COMMAND test_base64)
set_tests_properties(base64 PROPERTIES LABELS "unit")

add_executable(test_delta test_delta.c)
target_link_libraries(test_delta PRIVATE ably)
target_include_directories(test_delta PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME delta COMMAND test_delta)
set_tests_properties(delta PROPERTIES LABELS "unit")

add_executable(test_protocol_json test_protocol_json.c)
target_link_libraries(test_protocol_json PRIVATE ably)
target_include_directories(test_protocol_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME protocol_json COMMAND test_protocol_json)
set_tests_properties(protocol_json PROPERTIES LABELS "unit")

add_executable(test_protocol_msgpack test_protocol_msgpack.c)
target_link_libraries(test_protocol_msgpack PRIVATE ably)
target_include_directories(test_protocol_msgpack PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME protocol_msgpack COMMAND test_protocol_msgpack)
set_tests_properties(protocol_msgpack PROPERTIES LABELS "unit")

# ---------------------------------------------------------------------------
# Integration tests (require ABLY_API_KEY environment variable)
# ---------------------------------------------------------------------------

add_executable(test_rest test_rest.c)
target_link_libraries(test_rest PRIVATE ably)
add_test(NAME rest_integration COMMAND test_rest)
set_tests_properties(rest_integration PROPERTIES
    LABELS "integration"
    ENVIRONMENT "ABLY_API_KEY=${ABLY_API_KEY}"
)

add_executable(test_realtime test_realtime.c)
target_link_libraries(test_realtime PRIVATE ably)
add_test(NAME realtime_integration COMMAND test_realtime)
set_tests_properties(realtime_integration PROPERTIES
    LABELS "integration"
    ENVIRONMENT "ABLY_API_KEY=${ABLY_API_KEY}"
)

# ---------------------------------------------------------------------------
# End-to-end pub/sub test (runs realtime_subscribe + realtime_publish as
# separate OS processes; requires ABLY_API_KEY)
# ---------------------------------------------------------------------------

add_executable(test_e2e_pubsub test_e2e_pubsub.c)
add_test(NAME e2e_pubsub
    COMMAND test_e2e_pubsub $<TARGET_FILE_DIR:realtime_subscribe>
)
add_dependencies(test_e2e_pubsub realtime_subscribe realtime_publish)
set_tests_properties(e2e_pubsub PROPERTIES
    LABELS "integration"
    ENVIRONMENT "ABLY_API_KEY=${ABLY_API_KEY}"
    TIMEOUT 120
)

# ---------------------------------------------------------------------------
# End-to-end delta compression test (runs realtime_subscribe_delta +
# realtime_publish as separate OS processes; requires ABLY_API_KEY)
# ---------------------------------------------------------------------------

add_executable(test_e2e_delta test_e2e_delta.c)
add_test(NAME e2e_delta
    COMMAND test_e2e_delta $<TARGET_FILE_DIR:realtime_subscribe_delta>
)
add_dependencies(test_e2e_delta realtime_subscribe_delta realtime_publish)
set_tests_properties(e2e_delta PROPERTIES
    LABELS "integration"
    ENVIRONMENT "ABLY_API_KEY=${ABLY_API_KEY}"
    TIMEOUT 120
)
