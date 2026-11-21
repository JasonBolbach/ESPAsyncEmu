add_executable(loop_test loop_test.cpp)
target_link_libraries(loop_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME loop_test COMMAND loop_test)

add_executable(tcp_test tcp_test.cpp)
target_link_libraries(tcp_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME tcp_test COMMAND tcp_test)

add_executable(udp_test udp_test.cpp)
target_link_libraries(udp_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME udp_test COMMAND udp_test)

add_executable(web_test web_test.cpp)
target_link_libraries(web_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
target_include_directories(web_test PRIVATE ${PROJECT_SOURCE_DIR}/src)  # WebUtil.h (md5/base64 for the auth tests)
add_test(NAME web_test COMMAND web_test)

add_executable(realtime_test realtime_test.cpp)
target_link_libraries(realtime_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
target_include_directories(realtime_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME realtime_test COMMAND realtime_test)

add_executable(compat_test compat_test.cpp)
target_link_libraries(compat_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME compat_test COMMAND compat_test)

add_executable(limits_test limits_test.cpp)
target_link_libraries(limits_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME limits_test COMMAND limits_test)

# The same web/realtime suites again under device limits: tiny send buffers exercise
# the ack-driven flow control, strict headers check that the stock handlers register
# what they need, and the connection cap must not trip on normal use.
foreach(profile esp8266 esp8266-hb esp32)
    foreach(suite web_test realtime_test)
        add_test(NAME ${suite}_${profile} COMMAND ${suite})
        set_tests_properties(${suite}_${profile} PROPERTIES ENVIRONMENT "ESPASYNCEMU_PROFILE=${profile}")
    endforeach()
endforeach()

if(ESPASYNCEMU_ENABLE_JSON)
    add_executable(json_test json_test.cpp)
    target_link_libraries(json_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
    add_test(NAME json_test COMMAND json_test)
    foreach(profile esp8266 esp32)
        add_test(NAME json_test_${profile} COMMAND json_test)
        set_tests_properties(json_test_${profile} PROPERTIES ENVIRONMENT "ESPASYNCEMU_PROFILE=${profile}")
    endforeach()
endif()
