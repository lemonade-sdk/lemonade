cmake_minimum_required(VERSION 3.20)

# Use static runtime library on Windows to avoid DLL dependencies
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(lemon_cpp VERSION 1.0.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific configurations
if(WIN32)
    set(EXECUTABLE_NAME "lemonade-router")
    # Set Windows target version to Windows 10 for cpp-httplib v0.26.0
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    
    # Add security-hardening compiler flags for MSVC
    if(MSVC)
        # Control Flow Guard - prevents control flow hijacking
        add_compile_options(/guard:cf)
        # Buffer Security Check - stack buffer overflow detection
        add_compile_options(/GS)
        
        # Linker security flags
        add_link_options(
            /DYNAMICBASE      # Address Space Layout Randomization (ASLR)
            /NXCOMPAT         # Data Execution Prevention (DEP)
            /GUARD:CF         # Control Flow Guard
        )
    endif()
else()
    set(EXECUTABLE_NAME "lemonade-router")
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
set(CURL_STATIC_CRT ON CACHE INTERNAL "")  # Use static runtime on Windows
set(HTTP_ONLY ON CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(CURL_DISABLE_TESTS ON CACHE INTERNAL "")
# Completely disable zstd support in curl to avoid runtime dependency
set(CURL_ZSTD OFF CACHE INTERNAL "")
set(USE_ZSTD OFF CACHE INTERNAL "")
set(HAVE_ZSTD OFF CACHE INTERNAL "")
set(CURL_DISABLE_ZSTD ON CACHE INTERNAL "")
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
    server/main.cpp
    server/server.cpp
    server/router.cpp
    server/cli_parser.cpp
    server/model_manager.cpp
    server/wrapped_server.cpp
    server/streaming_proxy.cpp
    server/system_info.cpp
    server/utils/http_client.cpp
    server/utils/json_utils.cpp
    server/utils/process_manager.cpp
    server/utils/path_utils.cpp
    server/utils/wmi_helper.cpp
    server/backends/llamacpp_server.cpp
    server/backends/fastflowlm_server.cpp
    server/backends/ryzenaiserver.cpp
)

# Add version resource file on Windows
if(WIN32)
    list(APPEND SOURCES server/version.rc)
endif()

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
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ws2_32 wsock32 wbemuuid ole32 oleaut32)
    
    # Embed manifest file
    if(MSVC)
        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/server/lemonade.manifest"
        )
    endif()
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

# Copy zstd.dll from system PATH at build time (Windows only)
if(WIN32)
    add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
        COMMAND where.exe zstd.dll > "${CMAKE_BINARY_DIR}/zstd_path.txt" 2>NUL || echo "zstd.dll not found" > "${CMAKE_BINARY_DIR}/zstd_path.txt"
        COMMAND powershell -NoProfile -Command "if (Test-Path '${CMAKE_BINARY_DIR}/zstd_path.txt') { $zstdPath = Get-Content '${CMAKE_BINARY_DIR}/zstd_path.txt' -First 1 -ErrorAction SilentlyContinue; if ($zstdPath -and (Test-Path $zstdPath)) { Copy-Item $zstdPath '$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/zstd.dll' -Force; Write-Host 'Copied zstd.dll from' $zstdPath } }"
        COMMENT "Copying zstd.dll from system PATH"
        VERBATIM
    )
endif()

# Installation
install(TARGETS ${EXECUTABLE_NAME}
    RUNTIME DESTINATION bin
)

# Add tray application subdirectory
add_subdirectory(tray)
