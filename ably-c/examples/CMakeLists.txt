cmake_minimum_required(VERSION 3.16)

# C examples
foreach(example rest_publish realtime_subscribe realtime_publish realtime_subscribe_delta)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE ably)
endforeach()

# C++17 examples (only built when ABLY_BUILD_CPP is ON, which is the default)
if(ABLY_BUILD_CPP)
    foreach(example rest_publish realtime_subscribe realtime_publish)
        set(target "${example}_cpp")
        add_executable(${target} ${example}.cpp)
        target_link_libraries(${target} PRIVATE ably)
        set_target_properties(${target} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
        )
    endforeach()
endif()
