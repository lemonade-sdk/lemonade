cmake_minimum_required(VERSION 3.20)

# Use static runtime library on Windows to avoid DLL dependencies
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(lemon_cpp VERSION 9.0.3)

# ============================================================
# Electron source paths (used by both runtime and installers)
# ============================================================
get_filename_component(ELECTRON_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../../src/app" ABSOLUTE)
set(ELECTRON_APP_BUILD_DIR "${ELECTRON_APP_SOURCE_DIR}/dist-app")
if(WIN32)
    set(ELECTRON_APP_UNPACKED_DIR "${ELECTRON_APP_BUILD_DIR}/win-unpacked")
    set(ELECTRON_EXE_NAME "Lemonade.exe")
elseif(APPLE)
    set(ELECTRON_APP_UNPACKED_DIR "${ELECTRON_APP_BUILD_DIR}/mac")
    set(ELECTRON_EXE_NAME "Lemonade.app")
else()
    set(ELECTRON_APP_UNPACKED_DIR "${ELECTRON_APP_BUILD_DIR}/linux-unpacked")
    # Electron builder uses lowercase product name on Linux
    set(ELECTRON_EXE_NAME "lemonade")
endif()

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

# Disable OpenSSL in cpp-httplib (not needed for localhost HTTP server)
# All HTTPS downloads are handled by libcurl with native SSL backends:
#   - Windows: Schannel (native)
#   - macOS: SecureTransport (native)
#   - Linux: OpenSSL (system library, not bundled)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)

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

# zstd (BSD License) - required by httplib for compression
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.5
    SOURCE_SUBDIR build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# libcurl (curl license - MIT/X derivate)
set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(CURL_STATICLIB ON CACHE INTERNAL "")
set(HTTP_ONLY ON CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(CURL_DISABLE_TESTS ON CACHE INTERNAL "")
set(SKIP_INSTALL_ALL ON CACHE INTERNAL "")  # Don't install curl files

# Enable native SSL backends per platform
if(WIN32)
    set(CURL_STATIC_CRT ON CACHE INTERNAL "")  # Use static runtime on Windows
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
FetchContent_MakeAvailable(json CLI11 zstd)

# Create alias for httplib compatibility
# httplib expects zstd::libzstd but FetchContent builds libzstd_static
if(TARGET libzstd_static AND NOT TARGET zstd::libzstd)
    add_library(zstd::libzstd ALIAS libzstd_static)
endif()

# Prevent httplib from searching for system zstd (we're providing it via FetchContent)
set(ZSTD_FOUND TRUE CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

# Fetch curl but prevent it from installing
FetchContent_GetProperties(curl)
if(NOT curl_POPULATED)
    FetchContent_Populate(curl)
    # Prevent curl from installing any files
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${curl_SOURCE_DIR} ${curl_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(SKIP_INSTALL_ALL OFF CACHE BOOL "" FORCE)
endif()

# Fetch httplib but prevent it from installing
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
    FetchContent_Populate(httplib)
    set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${httplib_SOURCE_DIR} ${httplib_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

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
    
    set(WIX_PRODUCT_WXS "${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs")
    set(WIX_FRAGMENT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/installer/generate_electron_fragment.py")
    
    # Find WiX Toolset 5.0+ (unified 'wix' command) and Python for fragment generation
    find_program(WIX_EXECUTABLE wix)
    find_package(Python3 COMPONENTS Interpreter)
    
    if(WIX_EXECUTABLE)
        execute_process(
            COMMAND ${WIX_EXECUTABLE} --version
            OUTPUT_VARIABLE WIX_VERSION_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        message(STATUS "WiX Toolset found: ${WIX_EXECUTABLE}")
        message(STATUS "WiX version: ${WIX_VERSION_OUTPUT}")
        
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" WIX_SOURCE_DIR_NATIVE)
        file(TO_NATIVE_PATH "${WIX_PRODUCT_WXS}" WIX_PRODUCT_WXS_NATIVE)
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lemonade-server-minimal.msi" WIX_MINIMAL_OUTPUT_NATIVE)
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lemonade.msi" WIX_FULL_OUTPUT_NATIVE)
        file(TO_NATIVE_PATH "${ELECTRON_APP_UNPACKED_DIR}" WIX_ELECTRON_SOURCE_NATIVE)
        set(WIX_ELECTRON_FRAGMENT "${CMAKE_CURRENT_BINARY_DIR}/installer/ElectronAppFragment.wxs")
        file(TO_NATIVE_PATH "${WIX_ELECTRON_FRAGMENT}" WIX_ELECTRON_FRAGMENT_NATIVE)
        
        # Minimal installer (server only)
        add_custom_target(wix_installer_minimal
            COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (server only)..."
            COMMAND ${WIX_EXECUTABLE} build
                -arch x64
                -ext WixToolset.UI.wixext
                -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
                -d IncludeElectron=0
                -out "${WIX_MINIMAL_OUTPUT_NATIVE}"
                "${WIX_PRODUCT_WXS_NATIVE}"
            COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade-server-minimal.msi"
            DEPENDS ${EXECUTABLE_NAME}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Building WiX MSI installer (server only)"
        )
        
        set(WIX_INSTALLER_TARGETS wix_installer_minimal)
        
        if(Python3_FOUND)
            add_custom_target(wix_installer_full
                COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (with Electron app)..."
                COMMAND ${Python3_EXECUTABLE} ${WIX_FRAGMENT_SCRIPT}
                    --source "${ELECTRON_APP_UNPACKED_DIR}"
                    --output "${WIX_ELECTRON_FRAGMENT}"
                    --component-group "ElectronAppComponents"
                    --root-id "ElectronAppDir"
                    --path-variable "ElectronSourceDir"
                COMMAND ${WIX_EXECUTABLE} build
                    -arch x64
                    -ext WixToolset.UI.wixext
                    -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
                    -d IncludeElectron=1
                    -d ElectronSourceDir="${WIX_ELECTRON_SOURCE_NATIVE}"
                    -out "${WIX_FULL_OUTPUT_NATIVE}"
                    "${WIX_PRODUCT_WXS_NATIVE}"
                    "${WIX_ELECTRON_FRAGMENT_NATIVE}"
                COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade.msi"
                DEPENDS ${EXECUTABLE_NAME}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Building WiX MSI installer (with Electron app)"
            )
            
            if(TARGET electron-app)
                add_dependencies(wix_installer_full electron-app)
            endif()
            
            list(APPEND WIX_INSTALLER_TARGETS wix_installer_full)
        else()
            message(STATUS "Python 3 interpreter not found. Skipping the Electron-enabled MSI target 'wix_installer_full'.")
        endif()
        
        add_custom_target(wix_installers
            DEPENDS ${WIX_INSTALLER_TARGETS}
        )
        
        message(STATUS "WiX installer targets configured. Run 'cmake --build . --target wix_installer_minimal' or 'wix_installer_full'.")
    else()
        message(STATUS "WiX Toolset not found. MSI installers will not be available.")
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
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest"
        )
    endif()
endif()

# Copy resources at build time to both the build directory and the Release directory
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade/tools/server/static
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
file(COPY ${CMAKE_SOURCE_DIR}/../../src/lemonade_server/server_models.json
     DESTINATION ${CMAKE_BINARY_DIR}/resources)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/resources/backend_versions.json
     DESTINATION ${CMAKE_BINARY_DIR}/resources)

# Also copy to Release and Debug directories for Visual Studio multi-config
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/resources
        $<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/resources
    COMMENT "Copying resources to output directory"
)

# ============================================================
# Electron App Integration (Cross-Platform)
# ============================================================

# Find Node.js and npm for building the Electron app
find_program(NODE_EXECUTABLE node)
find_program(NPM_EXECUTABLE npm)

# Custom target to build the Electron app
if(NODE_EXECUTABLE AND NPM_EXECUTABLE)
    if(WIN32)
        set(ELECTRON_BUILD_TARGET "build:win")
    elseif(APPLE)
        set(ELECTRON_BUILD_TARGET "build:mac")
    else()
        set(ELECTRON_BUILD_TARGET "build:linux")
    endif()
    
    add_custom_target(electron-app
        COMMAND ${CMAKE_COMMAND} -E echo "Building Electron app for current platform..."
        COMMAND ${CMAKE_COMMAND} -E chdir "${ELECTRON_APP_SOURCE_DIR}" ${NPM_EXECUTABLE} install
        COMMAND ${CMAKE_COMMAND} -E chdir "${ELECTRON_APP_SOURCE_DIR}" ${NPM_EXECUTABLE} run ${ELECTRON_BUILD_TARGET}
        COMMENT "Building Electron app with npm"
        WORKING_DIRECTORY "${ELECTRON_APP_SOURCE_DIR}"
        VERBATIM
    )
    
    message(STATUS "Node.js found: ${NODE_EXECUTABLE}")
    message(STATUS "npm found: ${NPM_EXECUTABLE}")
    message(STATUS "Electron app can be built with: cmake --build . --target electron-app")
else()
    message(STATUS "Node.js or npm not found - Electron app build target disabled")
    message(STATUS "Install Node.js to enable: https://nodejs.org/")
endif()

# Custom command to copy Electron app files after building the C++ server
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Checking for Electron app at ${ELECTRON_APP_UNPACKED_DIR}..."
    COMMAND ${CMAKE_COMMAND}
        -DELECTRON_APP_UNPACKED_DIR=${ELECTRON_APP_UNPACKED_DIR}
        -DTARGET_DIR=$<TARGET_FILE_DIR:${EXECUTABLE_NAME}>
        -DELECTRON_EXE_NAME=${ELECTRON_EXE_NAME}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CopyElectronApp.cmake
    COMMENT "Copying Electron app if available"
    VERBATIM
)

# Add tray application subdirectory
add_subdirectory(tray)

# ============================================================
# CPack Configuration for .deb Package (Linux only)
# ============================================================
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Lemonade")
    set(CPACK_PACKAGE_CONTACT "lemonade@amd.com")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lemonade Local LLM Server")
    set(CPACK_PACKAGE_DESCRIPTION "A lightweight, high-performance local LLM server with support for multiple backends including llama.cpp, FastFlowLM, and RyzenAI.")
    
    # Debian-specific settings
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Lemonade Team <lemonade@amd.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    
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
    
    # Check if Electron app is available for full package
    option(INCLUDE_ELECTRON_APP "Include Electron app in deb package" OFF)
    
    if(INCLUDE_ELECTRON_APP AND EXISTS "${ELECTRON_APP_UNPACKED_DIR}/${ELECTRON_EXE_NAME}")
        message(STATUS "Electron app found at ${ELECTRON_APP_UNPACKED_DIR}")
        message(STATUS "Building full deb package with Electron app")
        
        # Full package name
        set(CPACK_PACKAGE_NAME "lemonade")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3, libz1, unzip, libgtk-3-0, libnotify4, libnss3, libxss1, libxtst6, xdg-utils, libatspi2.0-0, libsecret-1-0")
        set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")
        
        # Install Electron app
        install(DIRECTORY "${ELECTRON_APP_UNPACKED_DIR}/"
            DESTINATION share/lemonade-server/app
            USE_SOURCE_PERMISSIONS
            PATTERN "*.so" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        )
        
        # Create a launcher script for the Electron app
        file(WRITE "${CMAKE_BINARY_DIR}/lemonade-app"
"#!/bin/bash
# Lemonade App Launcher
SCRIPT_DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"
APP_DIR=\"/usr/local/share/lemonade-server/app\"

# Check user-local install first
if [ -d \"$HOME/.local/share/lemonade-server/app\" ]; then
    APP_DIR=\"$HOME/.local/share/lemonade-server/app\"
fi

exec \"$APP_DIR/lemonade\" \"$@\"
")
        install(PROGRAMS "${CMAKE_BINARY_DIR}/lemonade-app"
            DESTINATION bin
        )
        
        # Create .desktop file for the Electron app
        file(WRITE "${CMAKE_BINARY_DIR}/lemonade.desktop"
"[Desktop Entry]
Name=Lemonade
Comment=Lemonade Local LLM Server
Exec=/usr/local/bin/lemonade-app
Icon=lemonade
Terminal=false
Type=Application
Categories=Utility;Development;
StartupNotify=true
StartupWMClass=Lemonade
")
        install(FILES "${CMAKE_BINARY_DIR}/lemonade.desktop"
            DESTINATION share/applications
        )
        
        # Post-install script for full package
        # Note: CPack requires the script to be named 'postinst' exactly
        # We copy postinst-full to a temporary postinst file for CPack
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/postinst-full" 
                       "${CMAKE_CURRENT_BINARY_DIR}/postinst" COPYONLY)
        set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_BINARY_DIR}/postinst")
    else()
        # Minimal package (server only)
        set(CPACK_PACKAGE_NAME "lemonade-server-minimal")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3, libz1, unzip")
        set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")
        
        # Post-install script for minimal package
        set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/postinst")
        
        if(INCLUDE_ELECTRON_APP)
            message(WARNING "INCLUDE_ELECTRON_APP is ON but Electron app not found at ${ELECTRON_APP_UNPACKED_DIR}")
            message(WARNING "Building minimal deb package instead")
        endif()
    endif()
    
    include(CPack)
endif()
