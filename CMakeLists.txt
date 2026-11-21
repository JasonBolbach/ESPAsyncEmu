cmake_minimum_required(VERSION 3.20)
project(ESPAsyncEmu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ESPASYNCEMU_BUILD_TESTS "Build ESPAsyncEmu tests" ${PROJECT_IS_TOP_LEVEL})
option(ESPASYNCEMU_ENABLE_JSON "Fetch ArduinoJson and enable AsyncJson.h" ON)
option(ESPASYNCEMU_BUILD_EXAMPLES "Build examples and integrations" ${PROJECT_IS_TOP_LEVEL})

# --- Asio (standalone, header-only, Boost license) -------------------------
include(cmake/FetchAsio.cmake)
if(ESPASYNCEMU_ENABLE_JSON)
    include(cmake/FetchArduinoJson.cmake)
endif()

# --- Library ---------------------------------------------------------------
add_library(ESPAsyncEmu STATIC
    src/loop.cpp
    src/AsyncTCP.cpp
    src/AsyncUDP.cpp
    src/FS.cpp
    src/WebUtil.cpp
    src/WebRequest.cpp
    src/WebResponse.cpp
    src/WebServer.cpp
    src/AsyncEventSource.cpp
    src/AsyncWebSocket.cpp
)
add_library(ESPAsyncEmu::ESPAsyncEmu ALIAS ESPAsyncEmu)

target_include_directories(ESPAsyncEmu
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ESPAsyncEmu PRIVATE asio::asio)
if(ESPASYNCEMU_ENABLE_JSON)
    target_link_libraries(ESPAsyncEmu PUBLIC ESPAsyncEmu::ArduinoJson)
    target_compile_definitions(ESPAsyncEmu PUBLIC ESPASYNCEMU_HAS_ARDUINOJSON=1)
endif()

if(WIN32)
    # Winsock, needed by Asio; _WIN32_WINNT silences Asio's version warning.
    target_link_libraries(ESPAsyncEmu PUBLIC ws2_32 mswsock)
    target_compile_definitions(ESPAsyncEmu PRIVATE _WIN32_WINNT=0x0A00)
endif()

# --- Wiki (markdown -> HTML) -----------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(wiki
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wiki/build.py
        COMMENT "Compiling wiki to HTML"
        VERBATIM)
endif()

if(ESPASYNCEMU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ESPASYNCEMU_BUILD_EXAMPLES)
    if(ESPASYNCEMU_BUILD_TESTS)
        enable_testing()  # examples register smoke tests
    endif()
    add_subdirectory(examples)
    add_subdirectory(integrations)
endif()
