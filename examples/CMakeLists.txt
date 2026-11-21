foreach(ex tcp_echo_server http_hello_server tcp_client_demo udp_ping_pong web_server)
    add_executable(${ex} ${ex}.cpp)
    target_link_libraries(${ex} PRIVATE ESPAsyncEmu::ESPAsyncEmu)
endforeach()

target_compile_definitions(web_server PRIVATE WEB_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web_data")

# The self-contained demos double as smoke tests.
add_test(NAME example_tcp_client_demo COMMAND tcp_client_demo)
add_test(NAME example_udp_ping_pong COMMAND udp_ping_pong)
