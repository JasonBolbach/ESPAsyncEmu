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
