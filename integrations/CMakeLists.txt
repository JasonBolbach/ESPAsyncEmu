add_subdirectory(arduino)

add_executable(frame_loop frame_loop/frame_loop.cpp)
target_link_libraries(frame_loop PRIVATE ESPAsyncEmu::ESPAsyncEmu)
add_test(NAME integration_frame_loop COMMAND frame_loop 60)

add_subdirectory(sdl2)
