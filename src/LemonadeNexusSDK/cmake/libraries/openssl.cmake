cmake_minimum_required(VERSION 3.25.1)

# =============================================================================
# OpenSSL — a pinned version, linked statically
# =============================================================================
# With LEMONADE_NEXUS_STATIC_DEPS=ON (the default for a top-level build), every
# target that uses OpenSSL links a pinned OpenSSL statically. A binary then
# carries no libssl/libcrypto dependency, so an OS update of OpenSSL, or of a
# library that the OS libcrypto loads (zlib, zstd), cannot change the code that
# Tier 1 attestation measures. An OpenSSL fix ships in a Nexus release that
# changes a pin below. Change a pin and its source in the same reviewed change.
#
#   Linux          The static libraries of Ubuntu 24.04 libssl-dev, at exactly
#                  LEMONADE_NEXUS_OPENSSL_VERSION_UBUNTU. release.yml installs
#                  the package from the Ubuntu snapshot UBUNTU_SNAPSHOT, so the
#                  package build is fixed too. Its libcrypto.a needs no zlib or
#                  zstd.
#   macOS arm64,   A static OpenSSL archive that .github/workflows/deps-openssl.yml
#   Windows x64    builds from the official source tarball and publishes on the
#                  release deps-openssl-<version> of this repository. CMake
#                  downloads it and checks its SHA-256 below. The build never
#                  compiles OpenSSL.
#
# LEMONADE_NEXUS_STATIC_DEPS=OFF uses a system OpenSSL (find_package), for
# example a distribution package. ON on a platform without a pin is an error.
#
# A project that includes this file without defining LEMONADE_NEXUS_STATIC_DEPS
# (a consumer that vendors it) gets the pinned archive on macOS arm64 and
# Windows x64, and a system OpenSSL on every other platform.
# =============================================================================

set(LEMONADE_NEXUS_OPENSSL_VERSION_UBUNTU "3.0.13")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION "3.5.9")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_URL
    "https://github.com/lemonade-sdk/lemonade-nexus/releases/download/deps-openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}")
# The SHA-256 values that the deps-openssl.yml run printed when it published
# the release deps-openssl-3.5.9.
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_MACOS_ARM64
    "39b2381d4c9578002375dd7f7a579e35a391f1146984befc6741cc42b934286c")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_WINDOWS_X64
    "2cb5bc100e1b95fe46297612eae985291cea730e0be0fd1a0bf1f64ca236131b")

# Skip entirely if targets already exist (e.g. from a parent project)
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    message(STATUS "OpenSSL targets already defined — skipping")
    return()
endif()

# ---------------------------------------------------------------------------
# Linux: the pinned Ubuntu static libraries
# ---------------------------------------------------------------------------
if(LEMONADE_NEXUS_STATIC_DEPS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OPENSSL_USE_STATIC_LIBS ON)
    find_package(OpenSSL ${LEMONADE_NEXUS_OPENSSL_VERSION_UBUNTU} EXACT QUIET)
    if(NOT OPENSSL_FOUND OR NOT OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$"
            OR NOT OPENSSL_SSL_LIBRARY MATCHES "\\.a$")
        message(FATAL_ERROR
            "LEMONADE_NEXUS_STATIC_DEPS=ON requires the static libraries of OpenSSL "
            "${LEMONADE_NEXUS_OPENSSL_VERSION_UBUNTU} (Ubuntu 24.04 libssl-dev). Found version "
            "'${OPENSSL_VERSION}', libraries '${OPENSSL_SSL_LIBRARY}' and "
            "'${OPENSSL_CRYPTO_LIBRARY}'. Install that libssl-dev, or configure with "
            "-DLEMONADE_NEXUS_STATIC_DEPS=OFF.")
    endif()
    message(STATUS "Using pinned OpenSSL ${OPENSSL_VERSION} (static): ${OPENSSL_CRYPTO_LIBRARY}")
    return()
endif()

# ---------------------------------------------------------------------------
# macOS and Windows: the pinned prebuilt archive
# ---------------------------------------------------------------------------
# The macOS archive holds arm64 code only. A universal or x86_64 build
# (CMAKE_OSX_ARCHITECTURES) has no pinned archive.
set(_LN_OPENSSL_PLATFORM "")
set(_LN_OPENSSL_APPLE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
if(NOT _LN_OPENSSL_APPLE_ARCH)
    set(_LN_OPENSSL_APPLE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(APPLE AND _LN_OPENSSL_APPLE_ARCH MATCHES "^(arm64|aarch64)$")
    set(_LN_OPENSSL_PLATFORM "macos-arm64")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_MACOS_ARM64}")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM")
    set(_LN_OPENSSL_PLATFORM "windows-x64")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_WINDOWS_X64}")
endif()

if(_LN_OPENSSL_PLATFORM AND (LEMONADE_NEXUS_STATIC_DEPS OR NOT DEFINED LEMONADE_NEXUS_STATIC_DEPS))
    set(_LN_OPENSSL_ARCHIVE
        "openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}-${_LN_OPENSSL_PLATFORM}.tar.gz")
    set(_LN_OPENSSL_DOWNLOAD "${CMAKE_BINARY_DIR}/_download/openssl/${_LN_OPENSSL_ARCHIVE}")
    set(_LN_OPENSSL_ROOT
        "${CMAKE_BINARY_DIR}/_deps/openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}-${_LN_OPENSSL_PLATFORM}")
    file(DOWNLOAD "${LEMONADE_NEXUS_OPENSSL_PREBUILT_URL}/${_LN_OPENSSL_ARCHIVE}"
         "${_LN_OPENSSL_DOWNLOAD}"
         EXPECTED_HASH SHA256=${_LN_OPENSSL_SHA256}
         TLS_VERIFY ON
         STATUS _LN_OPENSSL_STATUS)
    list(GET _LN_OPENSSL_STATUS 0 _LN_OPENSSL_STATUS_CODE)
    if(NOT _LN_OPENSSL_STATUS_CODE EQUAL 0)
        message(FATAL_ERROR "OpenSSL archive download failed: ${_LN_OPENSSL_STATUS}")
    endif()
    file(REMOVE_RECURSE "${_LN_OPENSSL_ROOT}")
    file(ARCHIVE_EXTRACT INPUT "${_LN_OPENSSL_DOWNLOAD}" DESTINATION "${_LN_OPENSSL_ROOT}")

    if(WIN32)
        set(_LN_SSL_LIB "${_LN_OPENSSL_ROOT}/lib/libssl.lib")
        set(_LN_CRYPTO_LIB "${_LN_OPENSSL_ROOT}/lib/libcrypto.lib")
    else()
        set(_LN_SSL_LIB "${_LN_OPENSSL_ROOT}/lib/libssl.a")
        set(_LN_CRYPTO_LIB "${_LN_OPENSSL_ROOT}/lib/libcrypto.a")
    endif()
    foreach(_ln_file IN ITEMS "${_LN_SSL_LIB}" "${_LN_CRYPTO_LIB}"
                              "${_LN_OPENSSL_ROOT}/include/openssl/opensslv.h")
        if(NOT EXISTS "${_ln_file}")
            message(FATAL_ERROR "The OpenSSL archive ${_LN_OPENSSL_ARCHIVE} has no ${_ln_file}")
        endif()
    endforeach()

    add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION             "${_LN_CRYPTO_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_LN_OPENSSL_ROOT}/include")
    add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION             "${_LN_SSL_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_LN_OPENSSL_ROOT}/include"
        INTERFACE_LINK_LIBRARIES      OpenSSL::Crypto)
    if(APPLE)
        set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES "-framework Security" "-framework CoreFoundation")
    else()
        set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES ws2_32 crypt32 advapi32 user32)
    endif()

    # Downstream find_package(OpenSSL) calls (httplib, jwt-cpp) resolve to these
    # targets through cmake/FindOpenSSL.cmake.
    set(OPENSSL_ROOT_DIR       "${_LN_OPENSSL_ROOT}" CACHE PATH "OpenSSL root (pinned archive)" FORCE)
    set(OPENSSL_INCLUDE_DIR    "${_LN_OPENSSL_ROOT}/include" CACHE PATH "OpenSSL include directory" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${_LN_CRYPTO_LIB}" CACHE FILEPATH "OpenSSL crypto library" FORCE)
    set(OPENSSL_SSL_LIBRARY    "${_LN_SSL_LIB}" CACHE FILEPATH "OpenSSL SSL library" FORCE)
    set(OPENSSL_FOUND TRUE)
    set(OPENSSL_VERSION "${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}")
    message(STATUS "Using pinned OpenSSL ${OPENSSL_VERSION} (static archive ${_LN_OPENSSL_ARCHIVE})")
    return()
endif()

# ---------------------------------------------------------------------------
# Everything else: a system OpenSSL
# ---------------------------------------------------------------------------
if(LEMONADE_NEXUS_STATIC_DEPS)
    message(FATAL_ERROR
        "LEMONADE_NEXUS_STATIC_DEPS=ON, but there is no pinned OpenSSL for this platform "
        "(${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}, CMAKE_OSX_ARCHITECTURES "
        "'${CMAKE_OSX_ARCHITECTURES}'). Pinned archives exist for macOS arm64 "
        "and Windows x64. Configure with -DLEMONADE_NEXUS_STATIC_DEPS=OFF to use a system OpenSSL.")
endif()

find_package(OpenSSL REQUIRED)
message(STATUS "Using system OpenSSL ${OPENSSL_VERSION}: ${OPENSSL_CRYPTO_LIBRARY}")
