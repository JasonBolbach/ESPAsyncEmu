# Arduino-style sketch runner.
#
#   espasyncemu_add_sketch(<target> <sketch.cpp> [more sources...])
#
# builds <sketch.cpp> against the Arduino.h shim and the desktop runner
# (which provides main()).
add_library(espasyncemu_arduino STATIC runner.cpp)
target_include_directories(espasyncemu_arduino PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(espasyncemu_arduino PUBLIC ESPAsyncEmu::ESPAsyncEmu)

function(espasyncemu_add_sketch target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE espasyncemu_arduino)
endfunction()

espasyncemu_add_sketch(echo_sketch sketches/echo_sketch.cpp)
add_test(NAME sketch_echo_runs COMMAND echo_sketch --run-for 300)

espasyncemu_add_sketch(web_sketch sketches/web_sketch.cpp)
add_test(NAME sketch_web_runs COMMAND web_sketch --run-for 600 --port-offset 32000)
