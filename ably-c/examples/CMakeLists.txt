cmake_minimum_required(VERSION 3.16)

foreach(example rest_publish realtime_subscribe realtime_publish realtime_subscribe_delta)
    add_executable(${example} ${example}.c)
    target_link_libraries(${example} PRIVATE ably)
endforeach()
