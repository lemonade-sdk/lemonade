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
#   Windows x64,   builds from the official source tarball and publishes on the
#   iOS, Android   release deps-openssl-<version> of this repository. CMake
#                  downloads it and checks its SHA-256 below. The build never
#                  compiles OpenSSL. The iOS and Android archives come from
#                  scripts/build-openssl-mobile.sh.
#
# LEMONADE_NEXUS_OPENSSL_ROOT, a directory with include/openssl and
# lib/lib{ssl,crypto}.a (for example the output of
# scripts/build-openssl-mobile.sh), replaces the download on any platform.
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
# Release that holds the archives: deps-openssl-<version>, or
# deps-openssl-<version>-r<n> for a later build of the same version.
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_RELEASE "deps-openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}-r2")
set(LEMONADE_NEXUS_OPENSSL_RELEASES_URL
    "https://github.com/lemonade-sdk/lemonade-nexus/releases/download")
# The SHA-256 values that the deps-openssl.yml run printed when it published
# the release deps-openssl-3.5.9-r2.
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_MACOS_ARM64
    "d3e3871a817cc30ddc4496f96e68134ac3c8a5a31d109b0a6df8871837b19f0a")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_WINDOWS_X64
    "8103d830bf47dfb6a67e858759155189cee47d6a3314a88af77428ff4041df96")
# The mobile archives are published on their own release so the desktop pins
# above stay unchanged. Empty until deps-openssl.yml publishes them; until then
# mobile builds pass LEMONADE_NEXUS_OPENSSL_ROOT.
set(LEMONADE_NEXUS_OPENSSL_MOBILE_RELEASE "deps-openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}-mobile-r1")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_IOS_ARM64 "")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_IOSSIMULATOR_ARM64 "")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_IOSSIMULATOR_X86_64 "")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_ANDROID_ARM64_V8A "")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_ANDROID_ARMEABI_V7A "")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_ANDROID_X86_64 "")

set(LEMONADE_NEXUS_OPENSSL_ROOT "" CACHE PATH
    "Local static OpenSSL (include/openssl, lib/libssl.a, lib/libcrypto.a) used instead of the pinned archive")

# Skip entirely if targets already exist (e.g. from a parent project)
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    message(STATUS "OpenSSL targets already defined — skipping")
    return()
endif()

# ---------------------------------------------------------------------------
# Linux: the pinned Ubuntu static libraries
# ---------------------------------------------------------------------------
if(LEMONADE_NEXUS_STATIC_DEPS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT LEMONADE_NEXUS_OPENSSL_ROOT)
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
# macOS, Windows, iOS and Android: the pinned prebuilt archive
# ---------------------------------------------------------------------------
# The macOS archive holds arm64 code only. A universal or x86_64 build
# (CMAKE_OSX_ARCHITECTURES) has no pinned archive. iOS is checked first:
# CMake sets APPLE there too.
set(_LN_OPENSSL_PLATFORM "")
set(_LN_OPENSSL_RELEASE "${LEMONADE_NEXUS_OPENSSL_PREBUILT_RELEASE}")
set(_LN_OPENSSL_APPLE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
if(NOT _LN_OPENSSL_APPLE_ARCH)
    set(_LN_OPENSSL_APPLE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_LN_OPENSSL_RELEASE "${LEMONADE_NEXUS_OPENSSL_MOBILE_RELEASE}")
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator" AND _LN_OPENSSL_APPLE_ARCH STREQUAL "x86_64")
        set(_LN_OPENSSL_PLATFORM "iossimulator-x86_64")
        set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_IOSSIMULATOR_X86_64}")
    elseif(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(_LN_OPENSSL_PLATFORM "iossimulator-arm64")
        set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_IOSSIMULATOR_ARM64}")
    else()
        set(_LN_OPENSSL_PLATFORM "ios-arm64")
        set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_IOS_ARM64}")
    endif()
elseif(ANDROID)
    set(_LN_OPENSSL_RELEASE "${LEMONADE_NEXUS_OPENSSL_MOBILE_RELEASE}")
    set(_LN_OPENSSL_PLATFORM "android-${ANDROID_ABI}")
    string(TOUPPER "${ANDROID_ABI}" _ln_abi)
    string(REPLACE "-" "_" _ln_abi "${_ln_abi}")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_ANDROID_${_ln_abi}}")
elseif(APPLE AND _LN_OPENSSL_APPLE_ARCH MATCHES "^(arm64|aarch64)$")
    set(_LN_OPENSSL_PLATFORM "macos-arm64")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_MACOS_ARM64}")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM")
    set(_LN_OPENSSL_PLATFORM "windows-x64")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_WINDOWS_X64}")
endif()

if(LEMONADE_NEXUS_OPENSSL_ROOT OR (_LN_OPENSSL_PLATFORM
        AND (LEMONADE_NEXUS_STATIC_DEPS OR NOT DEFINED LEMONADE_NEXUS_STATIC_DEPS)))
    if(LEMONADE_NEXUS_OPENSSL_ROOT)
        set(_LN_OPENSSL_ROOT "${LEMONADE_NEXUS_OPENSSL_ROOT}")
        set(_LN_OPENSSL_ARCHIVE "${LEMONADE_NEXUS_OPENSSL_ROOT}")
    else()
        if(NOT _LN_OPENSSL_SHA256)
            message(FATAL_ERROR
                "There is no published OpenSSL archive for ${_LN_OPENSSL_PLATFORM} yet. Build one with "
                "scripts/build-openssl-mobile.sh and pass its directory as -DLEMONADE_NEXUS_OPENSSL_ROOT=...")
        endif()
        set(_LN_OPENSSL_ARCHIVE
            "openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}-${_LN_OPENSSL_PLATFORM}.tar.gz")
        set(_LN_OPENSSL_DOWNLOAD "${CMAKE_BINARY_DIR}/_download/openssl/${_LN_OPENSSL_ARCHIVE}")
        set(_LN_OPENSSL_ROOT
            "${CMAKE_BINARY_DIR}/_deps/openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}-${_LN_OPENSSL_PLATFORM}")
        file(DOWNLOAD
             "${LEMONADE_NEXUS_OPENSSL_RELEASES_URL}/${_LN_OPENSSL_RELEASE}/${_LN_OPENSSL_ARCHIVE}"
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
    endif()

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
    elseif(WIN32)
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
