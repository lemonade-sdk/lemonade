# Try system packages first, fall back to FetchContent if not available
# Define minimum version requirements based on FetchContent git tags
set(MIN_NLOHMANN_JSON_VERSION "3.11.3")
set(MIN_CLI11_VERSION "2.4.2")
set(MIN_CURL_VERSION "8.5.0")
set(MIN_ZSTD_VERSION "1.5.5")
set(DOWNLOAD_ZSTD_VERSION "1.5.7")
set(MIN_HTTPLIB_VERSION "0.26.0")
set(MIN_LWS_VERSION "4.3.3")

find_package(PkgConfig QUIET)

# Try to find system packages with version requirements
find_package(nlohmann_json ${MIN_NLOHMANN_JSON_VERSION} QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(CURL QUIET libcurl>=${MIN_CURL_VERSION})
    pkg_check_modules(ZSTD QUIET libzstd>=${MIN_ZSTD_VERSION})
    pkg_check_modules(LWS QUIET libwebsockets>=${MIN_LWS_VERSION})
endif()

include(${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
if(NOT APPLE)
    detect_system_httplib(${MIN_HTTPLIB_VERSION})
else()
    set(HTTPLIB_FOUND FALSE)
endif()
find_path(CLI11_INCLUDE_DIRS "CLI/CLI.hpp")
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
set(USE_SYSTEM_LWS ${LWS_FOUND})

# On macOS, always statically link dependencies to avoid Homebrew dylib issues
if(APPLE)
    set(USE_SYSTEM_ZSTD OFF)
    set(USE_SYSTEM_CURL OFF)
    set(USE_SYSTEM_HTTPLIB OFF)
    set(USE_SYSTEM_LWS OFF)
endif()

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

if(USE_SYSTEM_LWS)
    if(LWS_VERSION)
        message(STATUS "Using system libwebsockets (version ${LWS_VERSION}, >= ${MIN_LWS_VERSION})")
    else()
        message(STATUS "Using system libwebsockets (>= ${MIN_LWS_VERSION})")
    endif()
else()
    message(STATUS "System libwebsockets not found or version < ${MIN_LWS_VERSION}, will use FetchContent")
endif()

# ============================================================
# Common dependency configurations
# ============================================================

# Common configurations for all dependencies
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")  # Build static libraries
# Link to MSVC library statically
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Fetch missing dependencies
if(NOT USE_SYSTEM_JSON OR NOT USE_SYSTEM_CURL OR NOT USE_SYSTEM_ZSTD OR NOT USE_SYSTEM_CLI11 OR NOT USE_SYSTEM_HTTPLIB OR NOT USE_SYSTEM_LWS)
    include(FetchContent)
endif()

if(APPLE)
    # brotli (MIT License) - required by httplib for compression on macOS
    FetchContent_Declare(brotli
        GIT_REPOSITORY https://github.com/google/brotli.git
        GIT_TAG ed738e842d2fbdf2d6459e39267a633c4a9b2f5d  # v1.1.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
endif()

# === nlohmann/json ===
if(NOT USE_SYSTEM_JSON)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03  # v3.11.3
        GIT_SHALLOW TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(json)
endif()

# === CLI11 ===
if(NOT USE_SYSTEM_CLI11)
    FetchContent_Declare(CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG 6c7b07a878ad834957b98d0f9ce1dbe0cb204fc9  # v2.4.2
        GIT_SHALLOW TRUE
    )
    set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
    set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(CLI11)
endif()

# === curl ===
if(NOT USE_SYSTEM_CURL)
    FetchContent_Declare(curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG 55b5fafb094ebe07ca8a5d4f79813c8b40670795  # curl-8_5_0
        GIT_SHALLOW TRUE
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
        GIT_TAG ac66b19e6bd6b83238bf008eecc1298105298532  # v1.5.7
        GIT_SHALLOW TRUE
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
        GIT_TAG fe332fa06bac76a1c6d402c08f414052999347da  # v0.47.0
        GIT_SHALLOW TRUE
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

# ============================================================
# Download digest verification dependency (mbedTLS)
# ============================================================
if(NOT TARGET lemonade-digest-crypto)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MBEDTLS QUIET mbedtls mbedx509 mbedcrypto)
    endif()

    add_library(lemonade-digest-crypto INTERFACE)

    if(MBEDTLS_FOUND)
        message(STATUS "Using system mbedtls for CLI HTTPS and digest verification")
        if(MBEDTLS_INCLUDE_DIRS)
            target_include_directories(lemonade-digest-crypto INTERFACE ${MBEDTLS_INCLUDE_DIRS})
        endif()
        if(MBEDTLS_LIBRARY_DIRS)
            target_link_directories(lemonade-digest-crypto INTERFACE ${MBEDTLS_LIBRARY_DIRS})
        endif()
        target_link_libraries(lemonade-digest-crypto INTERFACE ${MBEDTLS_LIBRARIES})
        if(MBEDTLS_CFLAGS_OTHER)
            target_compile_options(lemonade-digest-crypto INTERFACE ${MBEDTLS_CFLAGS_OTHER})
        endif()
    else()
        find_path(MBEDTLS_INCLUDE_DIR NAMES mbedtls/ssl.h)
        find_library(MBEDTLS_LIBRARY NAMES mbedtls)
        find_library(MBEDX509_LIBRARY NAMES mbedx509)
        find_library(MBEDCRYPTO_LIBRARY NAMES mbedcrypto)

        if(MBEDTLS_INCLUDE_DIR AND MBEDTLS_LIBRARY AND MBEDX509_LIBRARY AND MBEDCRYPTO_LIBRARY)
            message(STATUS "Using system mbedtls libraries for CLI HTTPS and digest verification")
            target_include_directories(lemonade-digest-crypto INTERFACE ${MBEDTLS_INCLUDE_DIR})
            target_link_libraries(lemonade-digest-crypto INTERFACE ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})
        else()
            if(FETCHCONTENT_FULLY_DISCONNECTED AND NOT FETCHCONTENT_SOURCE_DIR_MBEDTLS)
                message(FATAL_ERROR
                    "System mbedtls was not found and FetchContent is fully disconnected. "
                    "Install the distro development package (for Debian/Ubuntu: libmbedtls-dev) "
                    "or provide FETCHCONTENT_SOURCE_DIR_MBEDTLS.")
            endif()

            message(STATUS "System mbedtls not found; fetching mbedTLS for CLI HTTPS and digest verification")
            include(FetchContent)
            FetchContent_Declare(mbedtls
                GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
                GIT_TAG 107ea89daaefb9867ea9121002fbbdf926780e98
                EXCLUDE_FROM_ALL
            )
            set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
            set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
            set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
            set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
            set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(mbedtls)

            target_include_directories(lemonade-digest-crypto INTERFACE ${mbedtls_SOURCE_DIR}/include)
            if(TARGET MbedTLS::mbedtls AND TARGET MbedTLS::mbedx509 AND TARGET MbedTLS::mbedcrypto)
                target_link_libraries(lemonade-digest-crypto INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
            elseif(TARGET mbedtls AND TARGET mbedx509 AND TARGET mbedcrypto)
                target_link_libraries(lemonade-digest-crypto INTERFACE mbedtls mbedx509 mbedcrypto)
            else()
                message(FATAL_ERROR "mbedTLS FetchContent did not create expected targets")
            endif()
        endif()
    endif()
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
endif()

# ============================================================
# lemonade-httplib target (Single Source of Truth for HTTPS)
# ============================================================
add_library(lemonade-httplib INTERFACE)

if(NOT USE_SYSTEM_HTTPLIB)
    # Bundled cpp-httplib path: wraps httplib::httplib and attaches MbedTLS support
    target_link_libraries(lemonade-httplib INTERFACE httplib::httplib lemonade-digest-crypto)
    target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
    if(APPLE)
        find_library(SECURITY_FRAMEWORK Security REQUIRED)
        target_link_libraries(lemonade-httplib INTERFACE ${SECURITY_FRAMEWORK})
    endif()
else()
    # System cpp-httplib path
    if(HTTPLIB_INCLUDE_DIRS)
        target_include_directories(lemonade-httplib INTERFACE ${HTTPLIB_INCLUDE_DIRS})
    endif()

    if(TARGET httplib::httplib)
        target_link_libraries(lemonade-httplib INTERFACE httplib::httplib)
    elseif(HTTPLIB_LINK_LIBRARIES)
        target_link_libraries(lemonade-httplib INTERFACE ${HTTPLIB_LINK_LIBRARIES})
    else()
        target_link_libraries(lemonade-httplib INTERFACE ${HTTPLIB_LIBRARIES})
        target_link_directories(lemonade-httplib INTERFACE ${HTTPLIB_LIBRARY_DIRS})
    endif()

    if(UNIX AND NOT APPLE AND TARGET Threads::Threads)
        target_link_libraries(lemonade-httplib INTERFACE Threads::Threads)
    endif()

    # Propagate compiler options from pkg-config (e.g. -DCPPHTTPLIB_OPENSSL_SUPPORT)
    if(HTTPLIB_CFLAGS_OTHER)
        target_compile_options(lemonade-httplib INTERFACE ${HTTPLIB_CFLAGS_OTHER})
    endif()

    # Determine and propagate the TLS backend configured on system httplib
    set(SYSTEM_HTTPLIB_HAS_TLS FALSE)
    set(SYSTEM_HTTPLIB_USES_OPENSSL FALSE)
    set(SYSTEM_HTTPLIB_USES_MBEDTLS FALSE)
    set(SYSTEM_HTTPLIB_USES_WOLFSSL FALSE)
    set(SYSTEM_HTTPLIB_USES_SCHANNEL FALSE)

    # 1. Use exported configuration variables from find_package(httplib)
    if(HTTPLIB_IS_USING_OPENSSL)
        set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
        set(SYSTEM_HTTPLIB_USES_OPENSSL TRUE)
    elseif(HTTPLIB_IS_USING_MBEDTLS)
        set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
        set(SYSTEM_HTTPLIB_USES_MBEDTLS TRUE)
    elseif(HTTPLIB_IS_USING_WOLFSSL)
        set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
        set(SYSTEM_HTTPLIB_USES_WOLFSSL TRUE)
    endif()

    # 2. Fall back to checking pkg-config flags
    if(NOT SYSTEM_HTTPLIB_HAS_TLS AND HTTPLIB_CFLAGS_OTHER)
        if(HTTPLIB_CFLAGS_OTHER MATCHES "CPPHTTPLIB_OPENSSL_SUPPORT")
            set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
            set(SYSTEM_HTTPLIB_USES_OPENSSL TRUE)
        elseif(HTTPLIB_CFLAGS_OTHER MATCHES "CPPHTTPLIB_MBEDTLS_SUPPORT")
            set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
            set(SYSTEM_HTTPLIB_USES_MBEDTLS TRUE)
        elseif(HTTPLIB_CFLAGS_OTHER MATCHES "CPPHTTPLIB_WOLFSSL_SUPPORT")
            set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
            set(SYSTEM_HTTPLIB_USES_WOLFSSL TRUE)
        elseif(HTTPLIB_CFLAGS_OTHER MATCHES "CPPHTTPLIB_SCHANNEL_SUPPORT")
            set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
            set(SYSTEM_HTTPLIB_USES_SCHANNEL TRUE)
        endif()
    endif()

    # 3. Propagate definitions and link requirements
    if(SYSTEM_HTTPLIB_USES_OPENSSL)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        find_package(OpenSSL REQUIRED)
        target_link_libraries(lemonade-httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    elseif(SYSTEM_HTTPLIB_USES_MBEDTLS)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
        target_link_libraries(lemonade-httplib INTERFACE lemonade-digest-crypto)
        if(APPLE)
            find_library(SECURITY_FRAMEWORK Security REQUIRED)
            target_link_libraries(lemonade-httplib INTERFACE ${SECURITY_FRAMEWORK})
        endif()
    elseif(SYSTEM_HTTPLIB_USES_WOLFSSL)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_WOLFSSL_SUPPORT)
    elseif(SYSTEM_HTTPLIB_USES_SCHANNEL)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_SCHANNEL_SUPPORT)
        if(WIN32)
            target_link_libraries(lemonade-httplib INTERFACE crypt32 secur32)
        endif()
    endif()

    # 4. If neither target nor pkg-config explicitly defined a TLS backend, configure MbedTLS fallback for header-only
    if(NOT SYSTEM_HTTPLIB_HAS_TLS AND NOT HTTPLIB_IS_COMPILED)
        if(HTTPLIB_VERSION AND NOT HTTPLIB_VERSION VERSION_LESS "0.31.0")
            target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
            target_link_libraries(lemonade-httplib INTERFACE lemonade-digest-crypto)
            if(APPLE)
                find_library(SECURITY_FRAMEWORK Security REQUIRED)
                target_link_libraries(lemonade-httplib INTERFACE ${SECURITY_FRAMEWORK})
            endif()
        endif()
    endif()
endif()

# === libwebsockets (for WebSocket support) ===
if(NOT USE_SYSTEM_LWS)
    # Workaround: libwebsockets v4.3.3 uses cmake_minimum_required(VERSION 2.8.12)
    # which CMake 4.x rejects. Allow older policy versions for subdirectory builds.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
        cmake_policy(SET CMP0148 OLD)
    endif()
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    # pkg_check_modules(LWS ...) caches LWS_LIBRARIES even when the system lib
    # is unused; libwebsockets' own CMake unsets the directory variable, which
    # un-shadows the stale cache entry and duplicates the "websockets" target
    # in its export set — a hard error on CMake >= 4. Clear it before the fetch.
    unset(LWS_LIBRARIES CACHE)
    FetchContent_Declare(libwebsockets
        GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
        GIT_TAG 4415e84c095857629863804e941b9e1c2e9347ef  # v4.3.3
        GIT_SHALLOW TRUE
    )
    # Build as static library, disable features we don't need
    set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TEST_PING ON CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
    set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
    # Disable TLS for now (can be enabled later for WSS/network access)
    set(LWS_WITH_SSL OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_ZIP_FOPS OFF CACHE BOOL "" FORCE)
    # Disable -Werror for libwebsockets — newer GCC/Clang triggers warnings in upstream code
    # First populate to get the source
    FetchContent_Populate(libwebsockets)
    # Patch the CMakeLists.txt files to remove -Werror
    file(READ "${libwebsockets_SOURCE_DIR}/CMakeLists.txt" LWS_CMAKE_MAIN)
    # CRLF-safe: normalize CR before any line-based patching (autocrlf leaves \r\n on Windows)
    string(REPLACE "\r" "" LWS_CMAKE_MAIN "${LWS_CMAKE_MAIN}")
    string(REPLACE "-Werror" "" LWS_CMAKE_MAIN "${LWS_CMAKE_MAIN}")
    # remove directory from OUTPUT list (MSBuild interprets it as file path — MSB8065)
    string(REPLACE "\t\t\t\${PROJECT_BINARY_DIR}/include/libwebsockets\n" ""
           LWS_CMAKE_MAIN "${LWS_CMAKE_MAIN}")
    # copy_directory only adds/overwrites — wipe the destination first so stale
    # headers removed upstream don't survive a GENHDR rerun across a dep update.
    # Works on every generator (ADDITIONAL_CLEAN_FILES below is Make/Ninja only).
    if(NOT LWS_CMAKE_MAIN MATCHES "rm -rf.*include/libwebsockets")
        string(REPLACE
            "COMMAND \${CMAKE_COMMAND} -E copy_directory \${CMAKE_CURRENT_SOURCE_DIR}/include/libwebsockets/"
            "COMMAND \${CMAKE_COMMAND} -E rm -rf \${CMAKE_CURRENT_BINARY_DIR}/include/libwebsockets\n\t\tCOMMAND \${CMAKE_COMMAND} -E copy_directory \${CMAKE_CURRENT_SOURCE_DIR}/include/libwebsockets/"
            LWS_CMAKE_MAIN "${LWS_CMAKE_MAIN}")
    endif()
    file(WRITE "${libwebsockets_SOURCE_DIR}/CMakeLists.txt" "${LWS_CMAKE_MAIN}")
    file(READ "${libwebsockets_SOURCE_DIR}/lib/CMakeLists.txt" LWS_CMAKE_LIB)
    string(REPLACE "\r" "" LWS_CMAKE_LIB "${LWS_CMAKE_LIB}")
    string(REPLACE "-Werror" "" LWS_CMAKE_LIB "${LWS_CMAKE_LIB}")
    file(WRITE "${libwebsockets_SOURCE_DIR}/lib/CMakeLists.txt" "${LWS_CMAKE_LIB}")
    # Now add the subdirectory
    add_subdirectory(${libwebsockets_SOURCE_DIR} ${libwebsockets_BINARY_DIR})
    # Removing the directory from OUTPUT above also drops it from the GENHDR clean
    # rules. Re-attach it as an additional clean file so stale LWS headers don't
    # survive a `clean` target across a dependency update (copy_directory only
    # adds/overwrites, never deletes).
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.15")
        set_property(
            TARGET GENHDR
            APPEND
            PROPERTY ADDITIONAL_CLEAN_FILES
            "${libwebsockets_BINARY_DIR}/include/libwebsockets"
        )
    endif()
    message(STATUS "libwebsockets will be fetched (v${MIN_LWS_VERSION})")
endif()

# Use brotli for compression on macOS
if(APPLE)
    set(HTTPLIB_REQUIRE_BROTLI OFF CACHE INTERNAL "")           # Skip brotli checks
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")  # Skip brotli checks
    set(HTTPLIB_IS_USING_BROTLI ON CACHE INTERNAL "")           # brotli is provided
endif()
