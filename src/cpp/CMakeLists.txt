cmake_minimum_required(VERSION 3.20)
project(lemon_cpp VERSION 9.0.7)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ============================================================
# Common dependency declarations
# ============================================================

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
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_5_0
)

# zstd (BSD License) - required by httplib for compression
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.5
    SOURCE_SUBDIR build/cmake
)

# cpp-httplib (MIT License)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.26.0
)


# ============================================================
# Common dependency configurations
# ============================================================

# Common configurations for all dependencies
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")  # Build static libraries
# Link to MSVC library statically
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# === nlohmann/json ===
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

# === CLI11 ===
set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(CLI11)

# === curl ===
set(BUILD_STATIC_LIBS ON CACHE INTERNAL "")
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(CURL_DISABLE_TESTS ON CACHE INTERNAL "")
set(CURL_DISABLE_INSTALL ON CACHE INTERNAL "")
set(HTTP_ONLY ON CACHE INTERNAL "")  # Disable protocols other than HTTP/S
# Configure platform-specific SSL backends
if(WIN32)
    set(CURL_STATIC_CRT ON CACHE INTERNAL "")  # Use static runtime on Windows
    set(CURL_USE_SCHANNEL ON CACHE INTERNAL "")
    set(CMAKE_USE_SCHANNEL ON CACHE INTERNAL "")
elseif(APPLE)
    set(CURL_USE_SECTRANSP ON CACHE INTERNAL "")
else()
    set(CURL_USE_OPENSSL ON CACHE INTERNAL "")
endif()
# Fetch curl and prevent it from installing
FetchContent_GetProperties(curl)
if(NOT curl_POPULATED)
    FetchContent_Populate(curl)
    add_subdirectory(${curl_SOURCE_DIR} ${curl_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# === zstd ===
set(ZSTD_BUILD_STATIC ON CACHE INTERNAL "")
set(ZSTD_BUILD_PROGRAMS OFF CACHE INTERNAL "")
set(ZSTD_BUILD_SHARED OFF CACHE INTERNAL "")
set(ZSTD_BUILD_TESTS OFF CACHE INTERNAL "")
# Fetch zstd and prevent it from installing
# FetchContent_MakeAvailable(zstd) forces zstd installation, therefore not used
FetchContent_GetProperties(zstd)
if(NOT zstd_POPULATED)
    FetchContent_Populate(zstd)
    add_subdirectory(${zstd_SOURCE_DIR}/build/cmake ${zstd_BINARY_DIR} EXCLUDE_FROM_ALL)
    add_library(zstd::libzstd ALIAS libzstd_static)  # Alias for httplib
endif()

# === httplib ===
# Disable OpenSSL in httplib (not needed for localhost HTTP server)
# All HTTPS downloads are handled by libcurl with native SSL backends:
#   - Windows: Schannel (native)
#   - macOS: SecureTransport (native)
#   - Linux: OpenSSL (system library, not bundled)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE INTERNAL "")
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
# Use zstd for compression
set(HTTPLIB_REQUIRE_ZSTD OFF CACHE INTERNAL "")           # Skip zstd checks
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE INTERNAL "")  # Skip zstd checks
set(HTTPLIB_IS_USING_ZSTD ON CACHE INTERNAL "")           # zstd is provided
set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")                # Prevent installing
FetchContent_MakeAvailable(httplib)
# Patch httplib includes due to manual zstd fetching
target_include_directories(httplib INTERFACE
    ${zstd_SOURCE_DIR}/lib
    ${zstd_SOURCE_DIR}/lib/common
)


# ============================================================
# Application: lemonade-router
# ============================================================

set(EXECUTABLE_NAME "lemonade-router")


# ============================================================
# Compilation configurations
# ============================================================

# Common compiler definitions
# Enable httplib thread pool with 8 threads
add_compile_definitions(CPPHTTPLIB_THREAD_POOL_COUNT=8)

# Platform-specific compiler definitions
if(WIN32)
    # Set Windows target version to Windows 10 for httplib v0.26.0
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    if(MSVC)
        # Add security-hardening compiler flags for MSVC
        # Control Flow Guard - prevents control flow hijacking
        add_compile_options(/guard:cf)
        # Buffer Security Check - stack buffer overflow detection
        add_compile_options(/GS)
    endif()
endif()


# ============================================================
# Compilation
# ============================================================

# Generate version header from template
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lemon/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lemon/version.h
    @ONLY
)

# Generate manifest files from templates (Windows only)
if(WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/server/lemonade.manifest.in
        ${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest
        @ONLY
    )
    
    # ============================================================
    # WiX Toolset Configuration (Windows MSI Installer)
    # ============================================================
    
    # Configure WiX Product.wxs template with version
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/installer/Product.wxs.in
        ${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs
        @ONLY
    )
    
    # Find WiX Toolset 5.0+ (unified 'wix' command)
    find_program(WIX_EXECUTABLE wix)
    
    if(WIX_EXECUTABLE)
        # Get WiX version
        execute_process(
            COMMAND ${WIX_EXECUTABLE} --version
            OUTPUT_VARIABLE WIX_VERSION_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        message(STATUS "WiX Toolset found: ${WIX_EXECUTABLE}")
        message(STATUS "WiX version: ${WIX_VERSION_OUTPUT}")
        
        # Create custom target for building MSI installer using WiX 5.0
        add_custom_target(wix_installer
            COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer with WiX 5.0..."
            
            # WiX 5.0 uses 'wix build' command - single file, simple approach
            COMMAND ${WIX_EXECUTABLE} build
                -arch x64
                -ext WixToolset.UI.wixext
                -d SourceDir="${CMAKE_CURRENT_SOURCE_DIR}"
                -out "${CMAKE_CURRENT_SOURCE_DIR}/lemonade-server-minimal.msi"
                "${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs"
            
            COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade-server-minimal.msi"
            
            DEPENDS ${EXECUTABLE_NAME}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Building WiX MSI installer"
        )
        
        message(STATUS "WiX installer target 'wix_installer' configured. Run 'cmake --build . --target wix_installer' to build MSI.")
    else()
        message(STATUS "WiX Toolset not found. MSI installer will not be available.")
        message(STATUS "  Install WiX Toolset 5.0.2 from: https://github.com/wixtoolset/wix/releases/download/v5.0.2/wix-cli-x64.msi")
        message(STATUS "  Or visit: https://wixtoolset.org/")
    endif()
    
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
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


# ============================================================
# Linking configurations
# ============================================================

# Platform-specific linking options
if(WIN32)
    if(MSVC)
        # Linker security flags
        add_link_options(
            /DYNAMICBASE      # Address Space Layout Randomization (ASLR)
            /NXCOMPAT         # Data Execution Prevention (DEP)
            /GUARD:CF         # Control Flow Guard
        )
        # Embed manifest file
        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest"
        )
    endif()
endif()


# ============================================================
# Linking
# ============================================================

# Common linking
target_link_libraries(${EXECUTABLE_NAME} PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    libcurl
)

# Platform-specific linking
if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE
        ws2_32
        wsock32
        wbemuuid
        ole32
        oleaut32
    )
endif()


# ============================================================
# Resources
# ============================================================

# Copy resources to the build directory during configuration
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade/tools/server/static
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade_server/server_models.json
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/resources/backend_versions.json
     DESTINATION ${CMAKE_BINARY_DIR}/resources)

# Copy resources to the runtime directoriy after build
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/resources
        $<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/resources
    COMMENT "Copying resources to output directory"
)


# ============================================================
# Applications:
#   - lemonade-server
#   - lemonade-tray (Windows only)
#   - lemonade-log-viewer (Windows only)
# ============================================================

add_subdirectory(tray)


# ============================================================
# CPack Configuration for .deb Package (Linux only)
# ============================================================
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "lemonade-server-minimal")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Lemonade")
    set(CPACK_PACKAGE_CONTACT "lemonade@amd.com")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lemonade Local LLM Server")
    set(CPACK_PACKAGE_DESCRIPTION "A lightweight, high-performance local LLM server with support for multiple backends including llama.cpp, FastFlowLM, and RyzenAI.")
    
    # Debian-specific settings
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Lemonade Team <lemonade@amd.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3, libz1, unzip")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")
    
    # Installation paths - use ~/.local for user install (no sudo needed)
    # Set via environment: cmake -DCPACK_PACKAGING_INSTALL_PREFIX=$HOME/.local ..
    if(NOT DEFINED CPACK_PACKAGING_INSTALL_PREFIX)
        set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    endif()
    
    # Only install our executables (not curl/development files)
    # Note: lemonade-server installation is handled in tray/CMakeLists.txt
    install(TARGETS ${EXECUTABLE_NAME}
        RUNTIME DESTINATION bin
    )
    
    # Install resources
    install(DIRECTORY ${CMAKE_BINARY_DIR}/resources/
        DESTINATION share/lemonade-server/resources
    )
    
    # Post-install script to make directories writable
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/postinst")
    
    include(CPack)
endif()
