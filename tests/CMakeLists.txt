add_executable(loop_test loop_test.cpp)
target_link_libraries(loop_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME loop_test COMMAND loop_test)

add_executable(tcp_test tcp_test.cpp)
target_link_libraries(tcp_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME tcp_test COMMAND tcp_test)

add_executable(udp_test udp_test.cpp)
target_link_libraries(udp_test PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME udp_test COMMAND udp_test)
