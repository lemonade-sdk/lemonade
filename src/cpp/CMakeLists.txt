cmake_minimum_required(VERSION 3.28)

# Use static runtime library on Windows to avoid DLL dependencies
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(lemon_cpp VERSION 9.1.3)

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
    EXCLUDE_FROM_ALL
)

# zstd (BSD License) - required by httplib for compression
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    SOURCE_SUBDIR build/cmake
    EXCLUDE_FROM_ALL
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
# Fetch curl (EXCLUDE_FROM_ALL prevents installation)
FetchContent_MakeAvailable(curl)

# === zstd ===
set(ZSTD_BUILD_STATIC ON CACHE INTERNAL "")
set(ZSTD_BUILD_PROGRAMS OFF CACHE INTERNAL "")
set(ZSTD_BUILD_SHARED OFF CACHE INTERNAL "")
set(ZSTD_BUILD_TESTS OFF CACHE INTERNAL "")
# Fetch zstd (EXCLUDE_FROM_ALL prevents installation)
FetchContent_MakeAvailable(zstd)
add_library(zstd::libzstd ALIAS libzstd_static)  # Alias for httplib

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
    server/recipe_options.cpp
    server/utils/http_client.cpp
    server/utils/json_utils.cpp
    server/utils/process_manager.cpp
    server/utils/path_utils.cpp
    server/utils/wmi_helper.cpp
    server/backends/llamacpp_server.cpp
    server/backends/fastflowlm_server.cpp
    server/backends/ryzenaiserver.cpp
    server/backends/whisper_server.cpp
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
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3, libz1, unzip, libgtk-3-0, libnotify4, libnss3, libxss1, libxtst6, xdg-utils, libatspi2.0-0, libsecret-1-0, libasound2t64")
        set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")
        
        # Install Electron app
        install(DIRECTORY "${ELECTRON_APP_UNPACKED_DIR}/"
            DESTINATION share/lemonade-server/app
            USE_SOURCE_PERMISSIONS
            PATTERN "*.so" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
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
    
    # RPM specific variables defined within
    include(${CMAKE_CURRENT_SOURCE_DIR}/CPackRPM.cmake)
    include(CPack)
endif()
