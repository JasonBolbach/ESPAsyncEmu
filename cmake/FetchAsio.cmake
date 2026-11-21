# Fetch standalone Asio and expose it as the INTERFACE target asio::asio.
# Asio ships no CMake build, so we point SOURCE_SUBDIR at a non-existent
# directory to make FetchContent download without add_subdirectory().
include(FetchContent)

if(NOT TARGET asio::asio)
    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  no-cmake-here)
    FetchContent_MakeAvailable(asio)

    find_package(Threads REQUIRED)
    add_library(asio INTERFACE)
    add_library(asio::asio ALIAS asio)
    target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    target_link_libraries(asio INTERFACE Threads::Threads)
endif()
