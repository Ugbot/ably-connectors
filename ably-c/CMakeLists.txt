cmake_minimum_required(VERSION 3.16)

project(ably_c
    VERSION 0.1.0
    DESCRIPTION "Standalone C/C++ client library for Ably real-time and REST"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Language standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD   11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD   17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(ABLY_BUILD_TESTS    "Build test programs"              ON)
option(ABLY_BUILD_EXAMPLES "Build example programs"           ON)
option(ABLY_BUILD_CPP      "Check C++ binding compilation"    ON)
option(ABLY_SANITIZE       "Enable ASan + UBSan (debug)"      OFF)

# Wire encoding: at least one must be ON.
option(ABLY_ENCODING_JSON    "Compile JSON codec"             ON)
option(ABLY_ENCODING_MSGPACK "Compile MessagePack codec"      ON)

if(NOT ABLY_ENCODING_JSON AND NOT ABLY_ENCODING_MSGPACK)
    message(FATAL_ERROR "At least one of ABLY_ENCODING_JSON or ABLY_ENCODING_MSGPACK must be ON")
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /WX /wd4100)  # /wd4100 = unused parameter (common in callbacks)
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter       # callbacks commonly ignore parameters
        -Wno-deprecated-declarations
    )
    if(ABLY_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Version header
# ---------------------------------------------------------------------------
configure_file(
    cmake/ably_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/ably/ably_version.h
    @ONLY
)

# ---------------------------------------------------------------------------
# Vendored library: mbedTLS 3.6.4
#
# Compiled directly from source rather than through mbedTLS's own CMakeLists.
# The mbedTLS release tarball omits the mbedtls_framework Python submodule
# that its CMake requires; direct compilation avoids that dependency entirely.
# ---------------------------------------------------------------------------
file(GLOB MBEDTLS_SOURCES
    src/vendor/mbedtls/library/*.c
)

add_library(ably_mbedtls STATIC ${MBEDTLS_SOURCES})

target_include_directories(ably_mbedtls
    PUBLIC
        src/vendor/mbedtls/include
    PRIVATE
        src/vendor/mbedtls/library
)

# mbedTLS threading support: use pthreads on POSIX, native on Windows.
if(WIN32)
    target_compile_definitions(ably_mbedtls PUBLIC MBEDTLS_THREADING_WINDOWS)
    target_link_libraries(ably_mbedtls PUBLIC ws2_32)
else()
    target_compile_definitions(ably_mbedtls PUBLIC MBEDTLS_THREADING_PTHREAD)
    target_link_libraries(ably_mbedtls PUBLIC Threads::Threads)
endif()

target_compile_definitions(ably_mbedtls PUBLIC MBEDTLS_THREADING_C)

set_target_properties(ably_mbedtls PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Suppress warnings from vendored code — we are not maintaining it.
if(NOT MSVC)
    target_compile_options(ably_mbedtls PRIVATE
        -Wno-unused-function
        -Wno-unused-variable
        -Wno-deprecated-declarations
        -Wno-pedantic
    )
endif()

set(ABLY_MBEDTLS_TARGETS ably_mbedtls)

# ---------------------------------------------------------------------------
# Vendored library: wslay
# ---------------------------------------------------------------------------
add_library(ably_wslay STATIC
    src/vendor/wslay/lib/wslay_event.c
    src/vendor/wslay/lib/wslay_frame.c
    src/vendor/wslay/lib/wslay_net.c
    src/vendor/wslay/lib/wslay_queue.c
    src/vendor/wslay/lib/wslay_stack.c
)
target_include_directories(ably_wslay
    PUBLIC  src/vendor/wslay/include
    PRIVATE src/vendor/wslay/include/wslay  # wslay sources use relative includes
)
set_target_properties(ably_wslay PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Suppress -Wstrict-prototypes from wslay vendored code (K&R-style declarations).
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ably_wslay PRIVATE -Wno-strict-prototypes)
endif()
# wslay needs ssize_t on Windows
if(WIN32)
    target_compile_definitions(ably_wslay PRIVATE _SSIZE_T_DEFINED ssize_t=__int64)
endif()

# ---------------------------------------------------------------------------
# Vendored library: cJSON
# ---------------------------------------------------------------------------
add_library(ably_cjson STATIC
    src/vendor/cjson/cJSON.c
)
target_include_directories(ably_cjson PUBLIC src/vendor/cjson)
set_target_properties(ably_cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Vendored library: mpack
# ---------------------------------------------------------------------------
add_library(ably_mpack STATIC
    src/vendor/mpack/mpack-common.c
    src/vendor/mpack/mpack-expect.c
    src/vendor/mpack/mpack-node.c
    src/vendor/mpack/mpack-platform.c
    src/vendor/mpack/mpack-reader.c
    src/vendor/mpack/mpack-writer.c
)
target_include_directories(ably_mpack PUBLIC src/vendor/mpack)
set_target_properties(ably_mpack PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Core library sources
# ---------------------------------------------------------------------------
set(ABLY_SOURCES
    src/alloc.c
    src/log.c
    src/base64.c
    src/mutex.c
    src/tls_ca.c
    src/http/http_client.c
    src/ws/ws_client.c
    src/rest/rest_client.c
    src/realtime/realtime_client.c
    src/realtime/channel.c
    src/realtime/protocol.c
    src/delta/vcdiff.c
)
if(ABLY_ENCODING_JSON)
    list(APPEND ABLY_SOURCES src/realtime/protocol_json.c)
endif()
if(ABLY_ENCODING_MSGPACK)
    list(APPEND ABLY_SOURCES src/realtime/protocol_msgpack.c)
endif()

# ---------------------------------------------------------------------------
# Static library target
# ---------------------------------------------------------------------------
add_library(ably STATIC ${ABLY_SOURCES})

target_include_directories(ably
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/delta
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/cjson
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/mpack
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/wslay/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/mbedtls/include
)

target_link_libraries(ably
    PRIVATE
        ably_cjson
        ably_mpack
        ably_wslay
        ${ABLY_MBEDTLS_TARGETS}
        Threads::Threads
)

# Encoding defines visible to the library's own translation units.
if(ABLY_ENCODING_JSON)
    target_compile_definitions(ably PRIVATE ABLY_HAVE_JSON=1)
endif()
if(ABLY_ENCODING_MSGPACK)
    target_compile_definitions(ably PRIVATE ABLY_HAVE_MSGPACK=1)
endif()

if(WIN32)
    target_link_libraries(ably PRIVATE ws2_32)  # WinSock2
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS ably
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ably DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/ably/ably_version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ably)

# ---------------------------------------------------------------------------
# C++ binding compile check
# ---------------------------------------------------------------------------
if(ABLY_BUILD_CPP)
    add_library(ably_cpp_check OBJECT
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpp_binding_check.cpp
    )
    target_link_libraries(ably_cpp_check PRIVATE ably)
    set_target_properties(ably_cpp_check PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(ABLY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ABLY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
