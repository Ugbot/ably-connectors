cmake_minimum_required(VERSION 3.16)

# C examples
foreach(example
    rest_publish
    realtime_subscribe
    realtime_publish
    realtime_subscribe_delta
    realtime_presence
)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE ably)
endforeach()

# C++17 examples (only built when ABLY_BUILD_CPP is ON, which is the default)
if(ABLY_BUILD_CPP)
    foreach(example rest_publish realtime_subscribe realtime_publish realtime_presence)
        set(target "${example}_cpp")
        add_executable(${target} ${example}.cpp)
        target_link_libraries(${target} PRIVATE ably)
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
        )
    endforeach()

    # Asio glue: compiled as a standalone binary only when Boost.Asio is available.
    find_package(Boost QUIET COMPONENTS system)
    if(Boost_FOUND)
        add_executable(asio_glue asio_glue_main.cpp)
        target_compile_definitions(asio_glue PRIVATE ABLY_ASIO_MAIN)
        target_link_libraries(asio_glue PRIVATE ably Boost::system)
        set_target_properties(asio_glue PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
        )
    else()
        message(STATUS "Boost not found — asio_glue example will not be built")
    endif()
endif()

# libuv event-loop glue example (optional — requires libuv)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
endif()

if(TARGET PkgConfig::LIBUV)
    add_executable(libuv_glue libuv_glue.c)
    target_link_libraries(libuv_glue PRIVATE ably PkgConfig::LIBUV)
else()
    message(STATUS "libuv not found — libuv_glue example will not be built")
endif()
