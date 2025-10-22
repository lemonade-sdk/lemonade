cmake_minimum_required(VERSION 3.20)
project(lemon_cpp VERSION 1.0.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific configurations
if(WIN32)
    set(EXECUTABLE_NAME "lemonade")
    # Set Windows target version to Windows 10 for cpp-httplib v0.26.0
    add_compile_definitions(_WIN32_WINNT=0x0A00)
else()
    set(EXECUTABLE_NAME "lemonade")
endif()

# Enable cpp-httplib thread pool with 8 threads
# This MUST be defined before including httplib.h
add_compile_definitions(CPPHTTPLIB_THREAD_POOL_COUNT=8)

# Dependencies
include(FetchContent)

# Disable building tests and examples for dependencies
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")

# cpp-httplib (MIT License)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.26.0
)

# nlohmann/json (MIT License)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

# CLI11 (BSD 3-Clause License)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)

# libcurl (curl license - MIT/X derivate)
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(CURL_STATICLIB ON CACHE INTERNAL "")
set(HTTP_ONLY ON CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(CURL_DISABLE_TESTS ON CACHE INTERNAL "")
# Enable native SSL backends per platform
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE INTERNAL "")
    set(CMAKE_USE_SCHANNEL ON CACHE INTERNAL "")
elseif(APPLE)
    set(CURL_USE_SECTRANSP ON CACHE INTERNAL "")
else()
    set(CURL_USE_OPENSSL ON CACHE INTERNAL "")
endif()
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_5_0
)

# Make dependencies available
FetchContent_MakeAvailable(httplib json CLI11 curl)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Source files
set(SOURCES
    src/main.cpp
    src/server.cpp
    src/router.cpp
    src/cli_parser.cpp
    src/model_manager.cpp
    src/wrapped_server.cpp
    src/streaming_proxy.cpp
    src/utils/http_client.cpp
    src/utils/json_utils.cpp
    src/utils/process_manager.cpp
    src/backends/llamacpp_server.cpp
    src/backends/fastflowlm_server.cpp
)

# Create executable
add_executable(${EXECUTABLE_NAME} ${SOURCES})

# Link libraries
target_link_libraries(${EXECUTABLE_NAME} PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    libcurl
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ws2_32 wsock32)
endif()

# Copy resources at build time to both the build directory and the Release directory
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade/tools/server/static
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade_server/server_models.json
     DESTINATION ${CMAKE_BINARY_DIR}/resources)

# Also copy to Release and Debug directories for Visual Studio multi-config
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/resources
        $<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/resources
    COMMENT "Copying resources to output directory"
)

# Installation
install(TARGETS ${EXECUTABLE_NAME}
    RUNTIME DESTINATION bin
)

