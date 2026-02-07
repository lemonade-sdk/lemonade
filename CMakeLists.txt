cmake_minimum_required(VERSION 3.10...3.28)

# Use static runtime library on Windows to avoid DLL dependencies
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(lemon_cpp VERSION 9.3.1)

# Enable Objective-C++ for macOS Metal support
if(APPLE)
    enable_language(OBJCXX)
endif()

# Prevent finding Homebrew libraries on macOS to use system ones
if(APPLE)
    set(CMAKE_IGNORE_PATH "/opt/homebrew/lib" "/opt/homebrew/include")
endif()
# Set default install prefix to /opt on Linux if not specified
if(UNIX AND NOT APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/opt" CACHE PATH "Install prefix" FORCE)
endif()

# Include GNUInstallDirs for standard installation directories
include(GNUInstallDirs)
# ============================================================
# Electron source paths (used by both runtime and installers)
# ============================================================
get_filename_component(ELECTRON_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/app" ABSOLUTE)
set(ELECTRON_APP_BUILD_DIR "${CMAKE_BINARY_DIR}/app")
if(WIN32)
    set(ELECTRON_APP_UNPACKED_DIR "${ELECTRON_APP_BUILD_DIR}/win-unpacked")
    set(ELECTRON_EXE_NAME "Lemonade.exe")
elseif(APPLE)
    set(ELECTRON_APP_UNPACKED_DIR "${ELECTRON_APP_BUILD_DIR}/mac-arm64")
    set(ELECTRON_EXE_NAME "Lemonade.app")
else()
    set(ELECTRON_APP_UNPACKED_DIR "${ELECTRON_APP_BUILD_DIR}/linux-unpacked")
    # Electron builder uses lowercase product name on Linux
    set(ELECTRON_EXE_NAME "lemonade")
endif()

# ============================================================
# Web App source paths
# ============================================================
get_filename_component(WEB_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/web-app" ABSOLUTE)
set(WEB_APP_BUILD_DIR "${CMAKE_BINARY_DIR}/resources/web-app")

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Try system packages first, fall back to FetchContent if not available
# Define minimum version requirements based on FetchContent git tags
set(MIN_NLOHMANN_JSON_VERSION "3.11.3")
set(MIN_CLI11_VERSION "2.4.2")
set(MIN_CURL_VERSION "8.5.0")
set(MIN_ZSTD_VERSION "1.5.7")
set(MIN_HTTPLIB_VERSION "0.26.0")

find_package(PkgConfig QUIET)

# Try to find system packages with version requirements
find_package(nlohmann_json ${MIN_NLOHMANN_JSON_VERSION} QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(CURL QUIET libcurl>=${MIN_CURL_VERSION})
    pkg_check_modules(ZSTD QUIET libzstd>=${MIN_ZSTD_VERSION})
    pkg_check_modules(HTTPLIB QUIET cpp-httplib>=${MIN_HTTPLIB_VERSION})
endif()
find_path(CLI11_INCLUDE_DIRS "CLI/CLI.hpp")
find_path(HTTPLIB_INCLUDE_DIRS "httplib.h")
# Verify CLI11 version if found
if(CLI11_INCLUDE_DIRS)
    if(EXISTS "${CLI11_INCLUDE_DIRS}/CLI/Version.hpp")
        file(STRINGS "${CLI11_INCLUDE_DIRS}/CLI/Version.hpp" CLI11_VERSION_LINE REGEX "^#define CLI11_VERSION ")
        if(CLI11_VERSION_LINE)
            string(REGEX REPLACE "^#define CLI11_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" "\\1" CLI11_VERSION "${CLI11_VERSION_LINE}")
            if(CLI11_VERSION VERSION_LESS MIN_CLI11_VERSION)
                message(STATUS "System CLI11 version ${CLI11_VERSION} is less than required ${MIN_CLI11_VERSION}")
                unset(CLI11_INCLUDE_DIRS)
            endif()
        endif()
    endif()
endif()

# Determine which dependencies need to be fetched
set(USE_SYSTEM_JSON ${nlohmann_json_FOUND})
set(USE_SYSTEM_CURL ${CURL_FOUND})
set(USE_SYSTEM_ZSTD ${ZSTD_FOUND})
set(USE_SYSTEM_CLI11 ${CLI11_INCLUDE_DIRS})
set(USE_SYSTEM_HTTPLIB ${HTTPLIB_FOUND})

if(USE_SYSTEM_JSON)
    message(STATUS "Using system nlohmann_json (>= ${MIN_NLOHMANN_JSON_VERSION})")
else()
    message(STATUS "System nlohmann_json not found or version < ${MIN_NLOHMANN_JSON_VERSION}, will use FetchContent")
endif()

if(USE_SYSTEM_CURL)
    message(STATUS "Using system libcurl (version ${CURL_VERSION}, >= ${MIN_CURL_VERSION})")
else()
    message(STATUS "System libcurl not found or version < ${MIN_CURL_VERSION}, will use FetchContent")
endif()

if(USE_SYSTEM_ZSTD)
    message(STATUS "Using system zstd (version ${ZSTD_VERSION}, >= ${MIN_ZSTD_VERSION})")
else()
    message(STATUS "System zstd not found or version < ${MIN_ZSTD_VERSION}, will use FetchContent")
endif()

if(USE_SYSTEM_CLI11)
    if(CLI11_VERSION)
        message(STATUS "Using system CLI11 (version ${CLI11_VERSION}, >= ${MIN_CLI11_VERSION})")
    else()
        message(STATUS "Using system CLI11 (>= ${MIN_CLI11_VERSION})")
    endif()
else()
    message(STATUS "System CLI11 not found or version < ${MIN_CLI11_VERSION}, will use FetchContent")
endif()

if(USE_SYSTEM_HTTPLIB)
    if(HTTPLIB_VERSION)
        message(STATUS "Using system cpp-httplib (version ${HTTPLIB_VERSION}, >= ${MIN_HTTPLIB_VERSION})")
    else()
        message(STATUS "Using system cpp-httplib (>= ${MIN_HTTPLIB_VERSION})")
    endif()
else()
    message(STATUS "System cpp-httplib not found or version < ${MIN_HTTPLIB_VERSION}, will use FetchContent")
endif()

# ============================================================
# Common dependency configurations
# ============================================================

# Common configurations for all dependencies
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")  # Build static libraries
# Link to MSVC library statically
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Fetch missing dependencies
if(NOT USE_SYSTEM_JSON OR NOT USE_SYSTEM_CURL OR NOT USE_SYSTEM_ZSTD OR NOT USE_SYSTEM_CLI11 OR NOT USE_SYSTEM_HTTPLIB)
    include(FetchContent)
endif()

if(APPLE)
    # brotli (MIT License) - required by httplib for compression on macOS
    FetchContent_Declare(brotli
        GIT_REPOSITORY https://github.com/google/brotli.git
        GIT_TAG v1.1.0
        EXCLUDE_FROM_ALL
    )
endif()

# === nlohmann/json ===
if(NOT USE_SYSTEM_JSON)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v${MIN_NLOHMANN_JSON_VERSION}
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(json)
endif()

# === CLI11 ===
if(NOT USE_SYSTEM_CLI11)
    FetchContent_Declare(CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v${MIN_CLI11_VERSION}
    )
    set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
    set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(CLI11)
endif()

# === curl ===
if(NOT USE_SYSTEM_CURL)
    string(REPLACE "." "_" CURL_TAG_VERSION "${MIN_CURL_VERSION}")
    FetchContent_Declare(curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-${CURL_TAG_VERSION}
        EXCLUDE_FROM_ALL
    )
    set(BUILD_STATIC_LIBS ON CACHE INTERNAL "")
    set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
    set(CURL_DISABLE_TESTS ON CACHE INTERNAL "")
    set(CURL_DISABLE_INSTALL ON CACHE INTERNAL "")
    set(HTTP_ONLY ON CACHE INTERNAL "")
    if(WIN32)
        set(CURL_STATIC_CRT ON CACHE INTERNAL "")
        set(CURL_USE_SCHANNEL ON CACHE INTERNAL "")
        set(CMAKE_USE_SCHANNEL ON CACHE INTERNAL "")
    elseif(APPLE)
        set(CURL_USE_SECTRANSP ON CACHE INTERNAL "")
    else()
        set(CURL_USE_OPENSSL ON CACHE INTERNAL "")
    endif()
    FetchContent_MakeAvailable(curl)
endif()

# === zstd ===
if(NOT USE_SYSTEM_ZSTD)
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v${MIN_ZSTD_VERSION}
        SOURCE_SUBDIR build/cmake
        EXCLUDE_FROM_ALL
    )
    set(ZSTD_BUILD_STATIC ON CACHE INTERNAL "")
    set(ZSTD_BUILD_PROGRAMS OFF CACHE INTERNAL "")
    set(ZSTD_BUILD_SHARED OFF CACHE INTERNAL "")
    set(ZSTD_BUILD_TESTS OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(zstd)
    add_library(zstd::libzstd ALIAS libzstd_static)
else()
    # Create an imported target for system zstd so httplib can find it
    if(NOT TARGET zstd::libzstd)
        add_library(zstd::libzstd UNKNOWN IMPORTED)
        # Find the actual library file
        find_library(ZSTD_LIBRARY NAMES zstd HINTS ${ZSTD_LIBRARY_DIRS})
        if(ZSTD_LIBRARY)
            set_target_properties(zstd::libzstd PROPERTIES
                IMPORTED_LOCATION "${ZSTD_LIBRARY}"
                IMPORTED_LOCATION_RELEASE "${ZSTD_LIBRARY}"
                IMPORTED_LOCATION_DEBUG "${ZSTD_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIRS}"
            )
        endif()
    endif()
endif()

if(APPLE)
    # === brotli ===
    set(BROTLI_BUILD_STATIC ON CACHE INTERNAL "")
    set(BROTLI_BUILD_PROGRAMS OFF CACHE INTERNAL "")
    set(BROTLI_BUILD_SHARED OFF CACHE INTERNAL "")
    set(BROTLI_BUILD_TESTS OFF CACHE INTERNAL "")

    FetchContent_MakeAvailable(brotli)

    if(TARGET brotlicommon)
        add_library(Brotli::common ALIAS brotlicommon)
    endif()

    if(TARGET brotlienc)
        add_library(Brotli::encoder ALIAS brotlienc)
    endif()

    if(TARGET brotlidec)
        add_library(Brotli::decoder ALIAS brotlidec)
    endif()
endif()

# === httplib ===
if(NOT USE_SYSTEM_HTTPLIB)
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v${MIN_HTTPLIB_VERSION}
    )
    set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_REQUIRE_ZSTD OFF CACHE INTERNAL "")
    set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE INTERNAL "")
    set(HTTPLIB_IS_USING_ZSTD ON CACHE INTERNAL "")
    set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(httplib)
    if(NOT USE_SYSTEM_ZSTD)
        target_include_directories(httplib INTERFACE
            ${zstd_SOURCE_DIR}/lib
            ${zstd_SOURCE_DIR}/lib/common
        )
    endif()
endif()


# Use brotli for compression on macOS
if(APPLE)
    set(HTTPLIB_REQUIRE_BROTLI OFF CACHE INTERNAL "")           # Skip brotli checks
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")  # Skip brotli checks
    set(HTTPLIB_IS_USING_BROTLI ON CACHE INTERNAL "")           # brotli is provided
endif()


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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/lemon/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lemon/version.h
    @ONLY
)

# Generate manifest files from templates (Windows only)
if(WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/lemonade.manifest.in
        ${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest
        @ONLY
    )

    # ============================================================
    # WiX Toolset Configuration (Windows MSI Installer)
    # ============================================================

    # Configure WiX Product.wxs template with version
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/installer/Product.wxs.in
        ${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs
        @ONLY
    )

    set(WIX_PRODUCT_WXS "${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs")
    set(WIX_FRAGMENT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/installer/generate_electron_fragment.py")

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
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp" WIX_CPP_SOURCE_DIR_NATIVE)
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" WIX_BUILD_DIR_NATIVE)
        file(TO_NATIVE_PATH "${ELECTRON_APP_UNPACKED_DIR}" WIX_ELECTRON_SOURCE_NATIVE)
        set(WIX_ELECTRON_FRAGMENT "${CMAKE_CURRENT_BINARY_DIR}/installer/ElectronAppFragment.wxs")
        file(TO_NATIVE_PATH "${WIX_ELECTRON_FRAGMENT}" WIX_ELECTRON_FRAGMENT_NATIVE)


        # Web app fragment paths
        file(TO_NATIVE_PATH "${WEB_APP_BUILD_DIR}" WIX_WEBAPP_SOURCE_NATIVE)
        set(WIX_WEBAPP_FRAGMENT "${CMAKE_CURRENT_BINARY_DIR}/installer/WebAppFragment.wxs")
        file(TO_NATIVE_PATH "${WIX_WEBAPP_FRAGMENT}" WIX_WEBAPP_FRAGMENT_NATIVE)

        # Minimal installer (server only, optionally with web-app if built)
        if(Python3_FOUND)
            add_custom_target(wix_installer_minimal
                COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (server only)..."
                # Generate web-app fragment if web-app directory exists
                COMMAND ${Python3_EXECUTABLE} ${WIX_FRAGMENT_SCRIPT}
                    --source "${WEB_APP_BUILD_DIR}"
                    --output "${WIX_WEBAPP_FRAGMENT}"
                    --component-group "WebAppComponents"
                    --root-id "WebAppDir"
                    --path-variable "WebAppSourceDir"
                    || ${CMAKE_COMMAND} -E echo "Web-app fragment generation skipped (web-app not built)"
                COMMAND ${WIX_EXECUTABLE} build
                    -arch x64
                    -ext WixToolset.UI.wixext
                    -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
                    -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
                    -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
                    -d IncludeElectron=0
                    -d IncludeWebApp=1
                    -d WebAppSourceDir="${WIX_WEBAPP_SOURCE_NATIVE}"
                    -out "${WIX_MINIMAL_OUTPUT_NATIVE}"
                    "${WIX_PRODUCT_WXS_NATIVE}"
                    "${WIX_WEBAPP_FRAGMENT_NATIVE}"
                    || ${WIX_EXECUTABLE} build
                        -arch x64
                        -ext WixToolset.UI.wixext
                        -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
                        -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
                        -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
                        -d IncludeElectron=0
                        -d IncludeWebApp=0
                        -out "${WIX_MINIMAL_OUTPUT_NATIVE}"
                        "${WIX_PRODUCT_WXS_NATIVE}"
                COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade-server-minimal.msi"
                DEPENDS ${EXECUTABLE_NAME}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Building WiX MSI installer (server only)"
            )
        else()
            add_custom_target(wix_installer_minimal
                COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (server only, no web-app)..."
                COMMAND ${WIX_EXECUTABLE} build
                    -arch x64
                    -ext WixToolset.UI.wixext
                    -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
                    -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
                    -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
                    -d IncludeElectron=0
                    -d IncludeWebApp=0
                    -out "${WIX_MINIMAL_OUTPUT_NATIVE}"
                    "${WIX_PRODUCT_WXS_NATIVE}"
                COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade-server-minimal.msi"
                DEPENDS ${EXECUTABLE_NAME}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "Building WiX MSI installer (server only)"
            )
        endif()

        set(WIX_INSTALLER_TARGETS wix_installer_minimal)
        # Add web-app dependency to minimal installer if target exists
        if(TARGET web-app)
            add_dependencies(wix_installer_minimal web-app)
        endif()

        if(Python3_FOUND)
            add_custom_target(wix_installer_full
                COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (with Electron app)..."
                # Generate web-app fragment
                COMMAND ${Python3_EXECUTABLE} ${WIX_FRAGMENT_SCRIPT}
                    --source "${WEB_APP_BUILD_DIR}"
                    --output "${WIX_WEBAPP_FRAGMENT}"
                    --component-group "WebAppComponents"
                    --root-id "WebAppDir"
                    --path-variable "WebAppSourceDir"
                    || ${CMAKE_COMMAND} -E echo "Warning: Web-app fragment generation failed"
                # Generate Electron app fragment
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
                    -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
                    -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
                    -d IncludeElectron=1
                    -d IncludeWebApp=1
                    -d ElectronSourceDir="${WIX_ELECTRON_SOURCE_NATIVE}"
                    -d WebAppSourceDir="${WIX_WEBAPP_SOURCE_NATIVE}"
                    -out "${WIX_FULL_OUTPUT_NATIVE}"
                    "${WIX_PRODUCT_WXS_NATIVE}"
                    "${WIX_ELECTRON_FRAGMENT_NATIVE}"
                    "${WIX_WEBAPP_FRAGMENT_NATIVE}"
                    || ${WIX_EXECUTABLE} build
                        -arch x64
                        -ext WixToolset.UI.wixext
                        -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
                        -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
                        -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
                        -d IncludeElectron=1
                        -d IncludeWebApp=0
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
            if(TARGET web-app)
                add_dependencies(wix_installer_full web-app)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Source files
set(SOURCES
    src/cpp/server/main.cpp
    src/cpp/server/server.cpp
    src/cpp/server/router.cpp
    src/cpp/server/cli_parser.cpp
    src/cpp/server/model_manager.cpp
    src/cpp/server/wrapped_server.cpp
    src/cpp/server/streaming_proxy.cpp
    src/cpp/server/system_info.cpp
    src/cpp/server/recipe_options.cpp
    src/cpp/server/utils/http_client.cpp
    src/cpp/server/utils/json_utils.cpp
    src/cpp/server/utils/process_manager.cpp
    src/cpp/server/utils/path_utils.cpp
    src/cpp/server/utils/wmi_helper.cpp
    src/cpp/server/utils/network_beacon.cpp
    src/cpp/server/backends/llamacpp_server.cpp
    src/cpp/server/backends/fastflowlm_server.cpp
    src/cpp/server/backends/ryzenaiserver.cpp
    src/cpp/server/backends/whisper_server.cpp
    src/cpp/server/backends/kokoro_server.cpp
    src/cpp/server/backends/sd_server.cpp
    src/cpp/server/backends/backend_utils.cpp
)

# Add platform-specific source files
if(APPLE)
    list(APPEND SOURCES src/cpp/server/macos_system_info.mm)
    set_source_files_properties(src/cpp/server/macos_system_info.mm PROPERTIES LANGUAGE OBJCXX)
endif()

# Add version resource file on Windows
if(WIN32)
    list(APPEND SOURCES src/cpp/server/version.rc)
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
if(USE_SYSTEM_JSON)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE nlohmann_json::nlohmann_json)
else()
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE nlohmann_json::nlohmann_json)
endif()

if(USE_SYSTEM_HTTPLIB)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE cpp-httplib)
else()
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE httplib::httplib)
endif()

if(USE_SYSTEM_CLI11)
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${CLI11_INCLUDE_DIRS})
else()
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE CLI11::CLI11)
endif()

if(USE_SYSTEM_CURL)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${CURL_LIBRARIES})
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
    target_compile_options(${EXECUTABLE_NAME} PRIVATE ${CURL_CFLAGS_OTHER})
else()
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE libcurl)
endif()

if(USE_SYSTEM_ZSTD)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${ZSTD_LIBRARIES})
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${ZSTD_INCLUDE_DIRS})
    target_link_directories(${EXECUTABLE_NAME} PRIVATE ${ZSTD_LIBRARY_DIRS})
    target_compile_options(${EXECUTABLE_NAME} PRIVATE ${ZSTD_CFLAGS_OTHER})
endif()

if(USE_SYSTEM_HTTPLIB AND HTTPLIB_INCLUDE_DIRS)
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${HTTPLIB_INCLUDE_DIRS})
endif()

# Enable ARC (Automatic Reference Counting) for macOS Objective-C++ files
if(APPLE)
    target_compile_options(${EXECUTABLE_NAME} PRIVATE -fobjc-arc)
endif()

# Platform-specific linking
if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE
        ws2_32
        wsock32
        wbemuuid
        ole32
        oleaut32
    )
elseif(APPLE)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework CoreServices"
        "-lobjc"
    )
    # Prevent linking to Homebrew OpenSSL on macOS, use Apple's built-in SSL
    target_link_options(${EXECUTABLE_NAME} PRIVATE -Wl,-no_weak_imports)
endif()

# ============================================================
# Resources
# ============================================================

# Copy resources to the build directory during configuration
# Resources are self-contained in src/cpp/resources/:
#   - static/index.html, static/favicon.ico (web UI)
#   - server_models.json (model registry)
#   - backend_versions.json (backend version configuration)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/resources/
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
# On Windows, npm is a batch file (npm.cmd), so we need to find it explicitly
# or invoke it through cmd.exe to avoid MSBuild custom build step issues
if(WIN32)
    find_program(NPM_EXECUTABLE npm.cmd)
else()
    find_program(NPM_EXECUTABLE npm)
endif()

# Custom target to build the Electron app (must be called manually)
if(NODE_EXECUTABLE AND NPM_EXECUTABLE)
    if(WIN32)
        set(ELECTRON_BUILD_TARGET "build:win")
        # On Windows, wrap npm calls with cmd /c to handle batch file execution properly
        set(NPM_COMMAND cmd /c "${NPM_EXECUTABLE}")
    elseif(APPLE)
        set(ELECTRON_BUILD_TARGET "build:mac")
        set(NPM_COMMAND "${NPM_EXECUTABLE}")
    else()
        set(ELECTRON_BUILD_TARGET "build:linux")
        set(NPM_COMMAND "${NPM_EXECUTABLE}")
    endif()

    set(ELECTRON_BUILD_SOURCE_DIR "${CMAKE_BINARY_DIR}/app-src")

    # Note: On Windows, NPM_COMMAND uses 'cmd /c' wrapper to handle npm.cmd batch file
    # Note: Avoid shell-special characters in echo messages (no parentheses, etc.)
    add_custom_target(electron-app
        COMMAND ${CMAKE_COMMAND} -E echo "Copying Electron app source to build directory..."
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${ELECTRON_BUILD_SOURCE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ELECTRON_BUILD_SOURCE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${ELECTRON_APP_SOURCE_DIR}" "${ELECTRON_BUILD_SOURCE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies..."
        COMMAND ${CMAKE_COMMAND} -E chdir "${ELECTRON_BUILD_SOURCE_DIR}" ${NPM_COMMAND} ci
        COMMAND ${CMAKE_COMMAND} -E echo "Building Electron app for current platform..."
        COMMAND ${CMAKE_COMMAND} -E chdir "${ELECTRON_BUILD_SOURCE_DIR}" ${NPM_COMMAND} run ${ELECTRON_BUILD_TARGET} -- --config.directories.output=${ELECTRON_APP_BUILD_DIR}
        COMMENT "Building Electron app with npm"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    message(STATUS "Node.js found: ${NODE_EXECUTABLE}")
    message(STATUS "npm found: ${NPM_EXECUTABLE}")
    if(WIN32)
        message(STATUS "Electron app can be built with: cmake --build --preset windows --target electron-app")
    else()
        message(STATUS "Electron app can be built with: cmake --build --preset default --target electron-app")
    endif()
else()
    message(STATUS "Node.js or npm not found - Electron app build target disabled")
    message(STATUS "Install Node.js to enable: https://nodejs.org/")
endif()

# ============================================================
# Web App Integration (Cross-Platform)
# ============================================================

# On Windows, disable automatic build by default (can be enabled with -DBUILD_WEB_APP=ON)
# On other platforms, enable automatic build by default (can be disabled with -DBUILD_WEB_APP=OFF)
if(WIN32)
    option(BUILD_WEB_APP "Build the Web app automatically" OFF)
else()
    option(BUILD_WEB_APP "Build the Web app automatically" ON)
endif()

# Custom target to build the Web app with proper dependency tracking
if(NODE_EXECUTABLE AND NPM_EXECUTABLE AND BUILD_WEB_APP)
    # Create a stamp file to track build state
    set(WEB_APP_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-app.stamp")
    set(WEB_APP_BUILD_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/web-app-src")

    # Get all source files in src/web-app (excluding node_modules and dist)
    file(GLOB_RECURSE WEB_APP_SOURCES
        CONFIGURE_DEPENDS
        "${WEB_APP_SOURCE_DIR}/src/*"
        "${WEB_APP_SOURCE_DIR}/*.json"
        "${WEB_APP_SOURCE_DIR}/*.js"
        "${WEB_APP_SOURCE_DIR}/*.ts"
        "${WEB_APP_SOURCE_DIR}/*.tsx"
        "${WEB_APP_SOURCE_DIR}/*.css"
    )

    # Add custom command to build web app (only runs if sources change)
    add_custom_command(
        OUTPUT ${WEB_APP_STAMP}
        COMMAND ${CMAKE_COMMAND}
            -DWEB_APP_SOURCE_DIR=${WEB_APP_SOURCE_DIR}
            -DWEB_APP_BUILD_SOURCE_DIR=${WEB_APP_BUILD_SOURCE_DIR}
            -DWEB_APP_BUILD_DIR=${WEB_APP_BUILD_DIR}
            -DNPM_EXECUTABLE=${NPM_EXECUTABLE}
            -DWEB_APP_STAMP=${WEB_APP_STAMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/src/web-app/BuildWebApp.cmake
        DEPENDS ${WEB_APP_SOURCES}
        COMMENT "Building Web app with npm"
    )

    # Add custom target that depends on the stamp file
    add_custom_target(web-app ALL DEPENDS ${WEB_APP_STAMP})

    message(STATUS "Web app will be built automatically (disable with -DBUILD_WEB_APP=OFF)")

    if (NOT WIN32 AND NOT APPLE AND NOT BUILD_ELECTRON_APP)
        # Install .desktop file for web app
        install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade-web-app.desktop
            DESTINATION share/applications
        )
        # Create symlink in standard applications path only if not installing to /usr
        if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
            install(CODE "
                file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/applications\")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        ${CMAKE_INSTALL_PREFIX}/share/applications/lemonade-web-app.desktop
                        \"\$ENV{DESTDIR}/usr/share/applications/lemonade-web-app.desktop\"
                )
            ")
        endif()
        install(FILES ${CMAKE_SOURCE_DIR}/src/web-app/assets/logo.svg
            DESTINATION share/pixmaps
            RENAME lemonade-app.svg
        )
        # Create symlink in standard pixmaps path only if not installing to /usr
        if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
            install(CODE "
                file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/pixmaps\")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        ${CMAKE_INSTALL_PREFIX}/share/pixmaps/lemonade-app.svg
                        \"\$ENV{DESTDIR}/usr/share/pixmaps/lemonade-app.svg\"
                )
            ")
        endif()
        # Install launch script for web app
        install(FILES ${CMAKE_SOURCE_DIR}/data/launch-lemonade-web-app.sh
            DESTINATION bin
            RENAME lemonade-web-app
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        )
        # Create symlink in standard bin path only if not installing to /usr
        if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
            install(CODE "
                file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        ${CMAKE_INSTALL_PREFIX}/bin/lemonade-web-app
                        \"\$ENV{DESTDIR}/usr/bin/lemonade-web-app\"
                )
            ")
        endif()
    endif()
else()
    if(NOT NODE_EXECUTABLE OR NOT NPM_EXECUTABLE)
        message(STATUS "Node.js or npm not found - Web app build target disabled")
    else()
        message(STATUS "Web app automatic build disabled (enable with -DBUILD_WEB_APP=ON)")
    endif()
endif()

# Add manual web-app target for platforms where automatic build is disabled
if(NODE_EXECUTABLE AND NPM_EXECUTABLE AND NOT BUILD_WEB_APP)
    add_custom_target(web-app
        COMMAND ${CMAKE_COMMAND} -E echo "Building Web app..."
        COMMAND ${CMAKE_COMMAND} -E chdir "${WEB_APP_SOURCE_DIR}" ${NPM_EXECUTABLE} install
        COMMAND ${CMAKE_COMMAND} -E chdir "${WEB_APP_SOURCE_DIR}" ${NPM_EXECUTABLE} run build
        COMMENT "Building Web app with npm"
        WORKING_DIRECTORY "${WEB_APP_SOURCE_DIR}"
        VERBATIM
    )
    message(STATUS "Web app can be built manually with: cmake --build . --target web-app")
endif()

# ============================================================
# macOS Packaging & Notarization
# ============================================================
if(APPLE)
    # SETUP IDENTITIES
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Lemonade")
    set(CPACK_PACKAGE_CONTACT "lemonade@amd.com")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lemonade Local LLM Server")
    set(CPACK_PACKAGE_DESCRIPTION "A lightweight, high-performance local LLM server.")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/../../LICENSE")
    # Skip readme page entirely
    set(CPACK_INSTALL_README_DIR "")

    if(NOT DEFINED SIGNING_IDENTITY)
        if(DEFINED ENV{DEVELOPER_ID_APPLICATION_IDENTITY})
            set(SIGNING_IDENTITY "$ENV{DEVELOPER_ID_APPLICATION_IDENTITY}")
        else()
            set(SIGNING_IDENTITY "BBC1E1924FF481293CF0928B5DFFF8A6419D7DA0")
        endif()
    endif()

    if(NOT DEFINED INSTALLER_IDENTITY)
        if(DEFINED ENV{DEVELOPER_ID_INSTALLER_IDENTITY})
            set(INSTALLER_IDENTITY "$ENV{DEVELOPER_ID_INSTALLER_IDENTITY}")
        else()
            set(INSTALLER_IDENTITY "B69167355F20D364EE8FD7126D767B93B5344EBB")
        endif()
    endif()

    # Configure Plists & Entitlements
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/com.lemonade.server.plist.in ${CMAKE_CURRENT_BINARY_DIR}/com.lemonade.server.plist @ONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/com.lemonade.tray.plist.in ${CMAKE_CURRENT_BINARY_DIR}/com.lemonade.tray.plist @ONLY)

    set(ENTITLEMENTS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/entitlements.plist")
    if(NOT EXISTS "${ENTITLEMENTS_PATH}")
        file(WRITE "${ENTITLEMENTS_PATH}"
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
            <!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
            <plist version=\"1.0\">
            <dict>
                <key>com.apple.security.cs.allow-jit</key><true/>
                <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
                <key>com.apple.security.cs.disable-library-validation</key><true/>
                <key>com.apple.security.cs.allow-dyld-environment-variables</key><true/>
            </dict>
            </plist>")
    endif()
endif()

# Add tray application subdirectory
add_subdirectory(src/cpp/tray)

if(APPLE)
    # SIGNING LOGIC (Release Only)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "Release Build: Configuring Hardened Runtime Signing for Router")

        # Sign 'lemonade-router'
        add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "--- Signing ${EXECUTABLE_NAME} ---"
            COMMAND codesign --force --options runtime --timestamp --entitlements "${ENTITLEMENTS_PATH}" --sign "${SIGNING_IDENTITY}" -v $<TARGET_FILE:${EXECUTABLE_NAME}>
            COMMENT "Signing ${EXECUTABLE_NAME} with Hardened Runtime"
            VERBATIM
        )
        # Note: 'lemonade-server' signs itself in tray/CMakeLists.txt
    endif()

    # Prepare Electron App
    set(ELECTRON_BUILD_COPY "${CMAKE_BINARY_DIR}/Lemonade.app")
    add_custom_target(prepare_electron_app
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${ELECTRON_BUILD_COPY}"
        COMMAND cp -R "${ELECTRON_APP_BUILD_DIR}/mac-arm64/Lemonade.app" "${CMAKE_BINARY_DIR}/" || cp -R "${ELECTRON_APP_BUILD_DIR}/mac/Lemonade.app" "${CMAKE_BINARY_DIR}/" || cp -R "${ELECTRON_APP_BUILD_DIR}/Lemonade.app" "${CMAKE_BINARY_DIR}/" || echo "Warning: Could not find Lemonade.app in ${ELECTRON_APP_BUILD_DIR}"
        COMMENT "Copying Electron App..."
        DEPENDS electron-app
    )

    set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/cpack_prebuild.cmake")
    file(WRITE "${CMAKE_BINARY_DIR}/cpack_prebuild.cmake"
        "message(STATUS \"Pre-build: Ensuring Electron app is prepared...\")\n"
        "execute_process(COMMAND ${CMAKE_COMMAND} --build \"${CMAKE_BINARY_DIR}\" --target prepare_electron_app)\n"
    )

    # INSTALLATION (Component Structure)

    # Main Router -> /usr/local/bin
    install(TARGETS ${EXECUTABLE_NAME} RUNTIME DESTINATION "bin" COMPONENT Runtime)

    # Sign the router binary after installation (Release builds only)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        install(CODE "
            execute_process(
                COMMAND codesign --force --options runtime --timestamp --entitlements \"${ENTITLEMENTS_PATH}\" --sign \"${SIGNING_IDENTITY}\" -v \"${CMAKE_BINARY_DIR}/_CPack_Packages/Darwin/productbuild/Lemonade-9.2.0-Darwin/Runtime/usr/local/bin/${EXECUTABLE_NAME}\"
                RESULT_VARIABLE SIGN_RESULT
            )
            if(NOT SIGN_RESULT EQUAL 0)
                message(FATAL_ERROR \"Failed to sign ${EXECUTABLE_NAME}\")
            endif()
        " COMPONENT Runtime)
    endif()

    # Tray/Server -> /usr/local/bin
    # (Since tray/CMakeLists.txt skips install on Apple, we must do it here)
    if(TARGET lemonade-server)
        install(TARGETS lemonade-server RUNTIME DESTINATION "bin" COMPONENT Runtime)

        # Sign the server binary after installation (Release builds only)
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            install(CODE "
                execute_process(
                    COMMAND codesign --force --options runtime --timestamp --entitlements \"${ENTITLEMENTS_PATH}\" --sign \"${SIGNING_IDENTITY}\" -v \"${CMAKE_BINARY_DIR}/_CPack_Packages/Darwin/productbuild/Lemonade-9.2.0-Darwin/Runtime/usr/local/bin/lemonade-server\"
                    RESULT_VARIABLE SIGN_RESULT
                )
                if(NOT SIGN_RESULT EQUAL 0)
                    message(FATAL_ERROR \"Failed to sign lemonade-server\")
                endif()
            " COMPONENT Runtime)
        endif()
    endif()

    # Resources -> /Library/Application Support/Lemonade/resources
    install(DIRECTORY ${CMAKE_BINARY_DIR}/resources/ DESTINATION "/Library/Application Support/Lemonade/resources" COMPONENT Resources)

    # App Bundle -> /Applications
    install(DIRECTORY "${ELECTRON_BUILD_COPY}"
        DESTINATION "/Applications"
        USE_SOURCE_PERMISSIONS
        COMPONENT Applications
    )

    # Plists -> /Library/...
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.lemonade.server.plist" DESTINATION "/Library/LaunchDaemons" COMPONENT Runtime)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.lemonade.tray.plist" DESTINATION "/Library/LaunchAgents" COMPONENT Runtime)

    # Placeholder -> /usr/local/share/lemonade-server
    file(WRITE "${CMAKE_BINARY_DIR}/.keep" "")
    install(FILES "${CMAKE_BINARY_DIR}/.keep" DESTINATION "usr/local/share/lemonade-server" COMPONENT Runtime)

    # ackaging & Notarization
    set(CPACK_GENERATOR "productbuild")
    set(CPACK_PACKAGE_NAME "Lemonade")
    set(CPACK_PRODUCTBUILD_IDENTIFIER "com.lemonade.server")

    set(CPACK_PRODUCTBUILD_DOMAIN "system")

    if(INSTALLER_IDENTITY)
        set(CPACK_PRODUCTBUILD_IDENTITY_NAME "${INSTALLER_IDENTITY}")
    endif()
    set(CPACK_PRODUCTBUILD_RELOCATE OFF)  # Prevent install_name_tool from invalidating signatures
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    set(CPACK_PRODUCTBUILD_BUNDLE com.lemonade.server.Applications OFF)
    set(CPACK_PRODUCTBUILD_BUNDLE_ID com.lemonade.server.Applications)

    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_SET_DESTDIR ON)
    set(CPACK_PACKAGE_RELOCATABLE OFF)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_COMPONENT_APPLICATIONS_IS_ABSOLUTE ON)

    # license file extension for CPack
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

    set(CPACK_POSTFLIGHT_SERVICES_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postinst-full-mac")
    include(CPack)

    set(PACKAGE_DEPENDS prepare_electron_app ${EXECUTABLE_NAME})

    add_custom_target(package-macos
        DEPENDS ${PACKAGE_DEPENDS}
        COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG> --target package
        COMMENT "Running CPack to create signed installer..."
        VERBATIM
    )

    find_program(XCRUN_EXE xcrun)
    if(XCRUN_EXE)
        set(PKG_NAME "Lemonade-${PROJECT_VERSION}-Darwin.pkg")
        add_custom_target(notarize
            COMMAND ${CMAKE_COMMAND} -E echo "Submitting ${PKG_NAME} for notarization..."
            COMMAND ${XCRUN_EXE} notarytool submit "${CMAKE_BINARY_DIR}/${PKG_NAME}" --keychain-profile "AC_PASSWORD" --wait --verbose
            COMMAND ${CMAKE_COMMAND} -E echo "Stapling ticket to ${PKG_NAME}..."
            COMMAND ${XCRUN_EXE} stapler staple "${CMAKE_BINARY_DIR}/${PKG_NAME}"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Notarizing and Stapling macOS package..."
            VERBATIM
        )
        add_dependencies(notarize package-macos)
    endif()
endif()

# ============================================================
# CPack Configuration for .deb Package (Linux only)
# ============================================================
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_SET_DESTDIR ON)
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
    # Default to /opt to match the install prefix used by the project.
    if(NOT DEFINED CPACK_PACKAGING_INSTALL_PREFIX)
        set(CPACK_PACKAGING_INSTALL_PREFIX "/opt")
    endif()

    # Only install our executables (not curl/development files)
    # Note: lemonade-server installation is handled in tray/CMakeLists.txt
    install(TARGETS ${EXECUTABLE_NAME}
        RUNTIME DESTINATION bin
    )

    # Install resources (all files including KaTeX fonts)
    install(DIRECTORY ${CMAKE_BINARY_DIR}/resources/
        DESTINATION share/lemonade-server/resources
    )

    # Configure systemd service file with correct paths
    configure_file(
        ${CMAKE_SOURCE_DIR}/data/lemonade-server.service.in
        ${CMAKE_BINARY_DIR}/lemonade-server.service
        @ONLY
    )

    # Install systemd service and configuration files
    install(FILES ${CMAKE_BINARY_DIR}/lemonade-server.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system
    )
    # Create symlink in standard systemd search path only if not installing to /usr
    # (if prefix is /usr, the service is already in the standard location)
    if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        install(CODE "
            file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/lib/systemd/system\")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    ${CMAKE_INSTALL_PREFIX}/lib/systemd/system/lemonade-server.service
                    \"\$ENV{DESTDIR}/usr/lib/systemd/system/lemonade-server.service\"
            )
        ")
    endif()
    install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade.conf
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/lemonade
    )

    # Create symlinks for KaTeX fonts if they exist on the system
    # Check if fonts-katex package fonts are available
    set(KATEX_FONTS_DIR "/usr/share/fonts/truetype/katex")
    if(EXISTS "${KATEX_FONTS_DIR}")
        message(STATUS "KaTeX fonts found at ${KATEX_FONTS_DIR}, creating symlinks")

        # Ensure web-app directory exists in build directory
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/resources/web-app")

        set(KATEX_FONTS
            "KaTeX_AMS-Regular.ttf" "KaTeX_AMS-Regular.woff" "KaTeX_AMS-Regular.woff2"
            "KaTeX_Caligraphic-Bold.ttf" "KaTeX_Caligraphic-Bold.woff" "KaTeX_Caligraphic-Bold.woff2"
            "KaTeX_Caligraphic-Regular.ttf" "KaTeX_Caligraphic-Regular.woff" "KaTeX_Caligraphic-Regular.woff2"
            "KaTeX_Fraktur-Bold.ttf" "KaTeX_Fraktur-Bold.woff" "KaTeX_Fraktur-Bold.woff2"
            "KaTeX_Fraktur-Regular.ttf" "KaTeX_Fraktur-Regular.woff" "KaTeX_Fraktur-Regular.woff2"
            "KaTeX_Main-Bold.ttf" "KaTeX_Main-Bold.woff" "KaTeX_Main-Bold.woff2"
            "KaTeX_Main-BoldItalic.ttf" "KaTeX_Main-BoldItalic.woff" "KaTeX_Main-BoldItalic.woff2"
            "KaTeX_Main-Italic.ttf" "KaTeX_Main-Italic.woff" "KaTeX_Main-Italic.woff2"
            "KaTeX_Main-Regular.ttf" "KaTeX_Main-Regular.woff" "KaTeX_Main-Regular.woff2"
            "KaTeX_Math-BoldItalic.ttf" "KaTeX_Math-BoldItalic.woff" "KaTeX_Math-BoldItalic.woff2"
            "KaTeX_Math-Italic.ttf" "KaTeX_Math-Italic.woff" "KaTeX_Math-Italic.woff2"
            "KaTeX_SansSerif-Bold.ttf" "KaTeX_SansSerif-Bold.woff" "KaTeX_SansSerif-Bold.woff2"
            "KaTeX_SansSerif-Italic.ttf" "KaTeX_SansSerif-Italic.woff" "KaTeX_SansSerif-Italic.woff2"
            "KaTeX_SansSerif-Regular.ttf" "KaTeX_SansSerif-Regular.woff" "KaTeX_SansSerif-Regular.woff2"
            "KaTeX_Script-Regular.ttf" "KaTeX_Script-Regular.woff" "KaTeX_Script-Regular.woff2"
            "KaTeX_Size1-Regular.ttf" "KaTeX_Size1-Regular.woff" "KaTeX_Size1-Regular.woff2"
            "KaTeX_Size2-Regular.ttf" "KaTeX_Size2-Regular.woff" "KaTeX_Size2-Regular.woff2"
            "KaTeX_Size3-Regular.ttf" "KaTeX_Size3-Regular.woff" "KaTeX_Size3-Regular.woff2"
            "KaTeX_Size4-Regular.ttf" "KaTeX_Size4-Regular.woff" "KaTeX_Size4-Regular.woff2"
            "KaTeX_Typewriter-Regular.ttf" "KaTeX_Typewriter-Regular.woff" "KaTeX_Typewriter-Regular.woff2"
        )

        foreach(font ${KATEX_FONTS})
            set(FONT_LINK "${CMAKE_BINARY_DIR}/resources/web-app/${font}")
            set(FONT_TARGET "${KATEX_FONTS_DIR}/${font}")
            # Only create symlink if the target font exists
            if(EXISTS "${FONT_TARGET}")
                # Remove existing file/symlink if present
                if(EXISTS "${FONT_LINK}" OR IS_SYMLINK "${FONT_LINK}")
                    file(REMOVE "${FONT_LINK}")
                endif()
                # Create symlink in build directory
                file(CREATE_LINK "${FONT_TARGET}" "${FONT_LINK}" SYMBOLIC)
            endif()
        endforeach()
    else()
        message(STATUS "KaTeX fonts not found at ${KATEX_FONTS_DIR}, using bundled fonts")
    endif()

    # Check if Electron app is available for full package
    option(BUILD_ELECTRON_APP "Build and include Electron app in deb package" OFF)

    # Build electron app if requested and target is available
    if(BUILD_ELECTRON_APP AND TARGET electron-app)
        message(STATUS "BUILD_ELECTRON_APP is ON - building Electron app...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target electron-app
            RESULT_VARIABLE ELECTRON_BUILD_RESULT
        )
        if(NOT ELECTRON_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build Electron app")
        endif()
    endif()

    if(BUILD_ELECTRON_APP AND EXISTS "${ELECTRON_APP_UNPACKED_DIR}/${ELECTRON_EXE_NAME}")
        message(STATUS "Electron app found at ${ELECTRON_APP_UNPACKED_DIR}")
        message(STATUS "Building full deb package with Electron app")

        # Full package name
        set(CPACK_PACKAGE_NAME "lemonade")

        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3, libz1, unzip, libgtk-3-0, libnotify4, libnss3, libxss1, libxtst6, xdg-utils, libatspi2.0-0, libsecret-1-0, libasound2t64, fonts-katex")
        set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")

        # Install Electron app
        install(DIRECTORY "${ELECTRON_APP_UNPACKED_DIR}/"
            DESTINATION share/lemonade-server/app
            USE_SOURCE_PERMISSIONS
            PATTERN "*.so" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        )

        if (NOT WIN32 AND NOT APPLE)
            if (EXISTS "${ELECTRON_APP_UNPACKED_DIR}/chrome-sandbox")
                install(FILES "${ELECTRON_APP_UNPACKED_DIR}/chrome-sandbox"
                    DESTINATION share/lemonade-server/app
                    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE SETUID
                )
            endif()

            if (EXISTS "${ELECTRON_APP_UNPACKED_DIR}/chrome_crashpad_handler")
                install(FILES "${ELECTRON_APP_UNPACKED_DIR}/chrome_crashpad_handler"
                    DESTINATION share/lemonade-server/app
                    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
                )
            endif()
        endif()

        if (NOT WIN32 AND NOT APPLE)
            # Install .desktop file for electron app
            install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade-app.desktop
                DESTINATION share/applications
            )
            # Create symlink in standard applications path only if not installing to /usr
            if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
                install(CODE "
                    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/applications\")
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} -E create_symlink
                            ${CMAKE_INSTALL_PREFIX}/share/applications/lemonade-app.desktop
                            \"\$ENV{DESTDIR}/usr/share/applications/lemonade-app.desktop\"
                    )
                ")
            endif()
            install(FILES ${CMAKE_SOURCE_DIR}/src/app/assets/logo.svg
                DESTINATION share/pixmaps
                RENAME lemonade-app.svg
            )

            # Create symlink in standard pixmaps path only if not installing to /usr
            if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
                install(CODE "
                    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/pixmaps\")
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} -E create_symlink
                            ${CMAKE_INSTALL_PREFIX}/share/pixmaps/lemonade-app.svg
                            \"\$ENV{DESTDIR}/usr/share/pixmaps/lemonade-app.svg\"
                    )
                ")
            endif()
            # Create symlink in standard bin path only if not installing to /usr
            if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
                install(CODE "
                    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} -E create_symlink
                            \"\${CMAKE_INSTALL_PREFIX}/share/lemonade-server/app/lemonade\"
                            \"\$ENV{DESTDIR}/usr/bin/lemonade-app\"
                    )
                ")
            else()
                install(CODE "
                    file(CREATE_LINK
                        \"\${CMAKE_INSTALL_PREFIX}/share/lemonade-server/app/lemonade\"
                        \"\${CMAKE_INSTALL_PREFIX}/bin/lemonade-app\"
                        SYMBOLIC)
                ")
            endif()
        endif()

    else()
        # Minimal package (server only)
        set(CPACK_PACKAGE_NAME "lemonade-server-minimal")

        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3, libz1, unzip, fonts-katex")
        set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb")

        if(BUILD_ELECTRON_APP)
            message(WARNING "BUILD_ELECTRON_APP is ON but Electron app not found at ${ELECTRON_APP_UNPACKED_DIR}")
            message(WARNING "Building minimal deb package instead")
        endif()
    endif()
    # Post-install script for debian package
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postinst")

    # RPM specific variables defined within
    include(${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/CPackRPM.cmake)
    include(CPack)
endif()
