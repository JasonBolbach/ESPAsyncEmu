# Fetch ArduinoJson (header-only, MIT) and expose it as ESPAsyncEmu::ArduinoJson.
#
# Used by AsyncJson.h. Choose the version with -DESPASYNCEMU_ARDUINOJSON_TAG=v6.21.6
# (v6 and v7 are both supported), point FETCHCONTENT_SOURCE_DIR_ARDUINOJSON at a local
# checkout to work offline, or disable it with -DESPASYNCEMU_ENABLE_JSON=OFF.
include(FetchContent)

if(NOT TARGET ESPAsyncEmu::ArduinoJson)
    set(ESPASYNCEMU_ARDUINOJSON_TAG "v7.4.3" CACHE STRING "ArduinoJson git tag to fetch (v6.21.x or v7.x)")
    FetchContent_Declare(arduinojson
        GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
        GIT_TAG        ${ESPASYNCEMU_ARDUINOJSON_TAG}
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  no-cmake-here)
    FetchContent_MakeAvailable(arduinojson)

    add_library(espasyncemu_arduinojson INTERFACE)
    add_library(ESPAsyncEmu::ArduinoJson ALIAS espasyncemu_arduinojson)
    # ArduinoJson.h lives in src/ (v6, v7); older layouts keep it at the root.
    target_include_directories(espasyncemu_arduinojson SYSTEM INTERFACE
        ${arduinojson_SOURCE_DIR}/src ${arduinojson_SOURCE_DIR})
    # Teach ArduinoJson about our Print / Stream (see Arduino.h). Not ARDUINO_STRING: our String
    # derives from std::string, which ArduinoJson already handles by duck typing; enabling its
    # Arduino String support as well would make the string reader ambiguous.
    target_compile_definitions(espasyncemu_arduinojson INTERFACE
        ARDUINOJSON_ENABLE_ARDUINO_STREAM=1
        ARDUINOJSON_ENABLE_ARDUINO_PRINT=1)
endif()
