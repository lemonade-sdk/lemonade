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
#   macOS, Windows A static OpenSSL archive that .github/workflows/deps-openssl.yml
#                  builds from the official source tarball and publishes on the
#                  release deps-openssl-<version> of this repository. CMake
#                  downloads it and checks its SHA-256 below.
#
# LEMONADE_NEXUS_STATIC_DEPS=OFF uses the legacy logic at the end of this file:
# a system OpenSSL, or the OPENSSL_FORCE_BUNDLED source build.
# =============================================================================

set(LEMONADE_NEXUS_OPENSSL_VERSION_UBUNTU "3.0.13")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION "3.5.9")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_URL
    "https://github.com/lemonade-sdk/lemonade-nexus/releases/download/deps-openssl-${LEMONADE_NEXUS_OPENSSL_PREBUILT_VERSION}")
# FOLLOW-UP: set these from the output of deps-openssl.yml. While a value is
# empty, that platform keeps the legacy logic below.
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_MACOS_ARM64 "")
set(LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_WINDOWS_X64 "")

option(OPENSSL_FORCE_BUNDLED "Always build OpenSSL from source, ignoring any system copy" OFF)

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
set(_LN_OPENSSL_PLATFORM "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_LN_OPENSSL_PLATFORM "macos-arm64")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_MACOS_ARM64}")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_LN_OPENSSL_PLATFORM "windows-x64")
    set(_LN_OPENSSL_SHA256 "${LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_WINDOWS_X64}")
endif()

if(LEMONADE_NEXUS_STATIC_DEPS AND _LN_OPENSSL_PLATFORM AND _LN_OPENSSL_SHA256)
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

if(LEMONADE_NEXUS_STATIC_DEPS AND _LN_OPENSSL_PLATFORM)
    message(STATUS "No pinned OpenSSL archive for ${_LN_OPENSSL_PLATFORM} yet; using the legacy "
                   "OpenSSL logic (see LEMONADE_NEXUS_OPENSSL_PREBUILT_SHA256_*)")
endif()

# =============================================================================
# Legacy: prefer a system/package-manager OpenSSL, build from source as fallback
# =============================================================================
# CI installs OpenSSL with vcpkg on Windows. On macOS, OPENSSL_FORCE_BUNDLED=ON
# builds it statically from source so distributed binaries carry no Homebrew
# (or other /usr/local) dylib dependencies.
# =============================================================================

# ---------------------------------------------------------------------------
# Try system OpenSSL first (respects OPENSSL_ROOT_DIR if set)
# ---------------------------------------------------------------------------
if(NOT OPENSSL_FORCE_BUNDLED)
    find_package(OpenSSL QUIET)

    if(OPENSSL_FOUND AND TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        message(STATUS "Using system OpenSSL ${OPENSSL_VERSION}")
        return()
    endif()
endif()

# ---------------------------------------------------------------------------
# System OpenSSL not found — build 3.3.2 from source via ExternalProject_Add
# ---------------------------------------------------------------------------
if(OPENSSL_FORCE_BUNDLED)
    message(STATUS "OPENSSL_FORCE_BUNDLED=ON — building OpenSSL from source")
else()
    message(STATUS "System OpenSSL not found — building from source")
endif()

include(ExternalProject)

set(OPENSSL_VERSION  "3.3.2")
set(OPENSSL_URL      "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz")

# Compile target-filesystem paths into libcrypto and stage the install with
# DESTDIR: a build-tree --prefix baked the build directory into the final ELF
# and made it non-reproducible.
set(OPENSSL_STAGE_ROOT "${CMAKE_BINARY_DIR}/openssl-stage")
set(OPENSSL_INSTALL_PREFIX "${OPENSSL_STAGE_ROOT}/usr")

# ---------------------------------------------------------------------------
# Platform-specific Configure target
# ---------------------------------------------------------------------------
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(OPENSSL_CONFIGURE_TARGET "darwin64-arm64-cc")
    else()
        set(OPENSSL_CONFIGURE_TARGET "darwin64-x86_64-cc")
    endif()
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OPENSSL_CONFIGURE_TARGET "VC-WIN64A")
    else()
        set(OPENSSL_CONFIGURE_TARGET "VC-WIN32")
    endif()
else()
    # Linux / generic Unix
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(OPENSSL_CONFIGURE_TARGET "linux-x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(OPENSSL_CONFIGURE_TARGET "linux-aarch64")
    else()
        # Fallback — let OpenSSL's Configure auto-detect
        set(OPENSSL_CONFIGURE_TARGET "")
    endif()
endif()

# ---------------------------------------------------------------------------
# Determine parallel job count for make
# ---------------------------------------------------------------------------
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 1)
endif()

# ---------------------------------------------------------------------------
# Platform-specific build commands
# ---------------------------------------------------------------------------
if(WIN32)
    set(_OPENSSL_CONFIGURE_CMD
        perl Configure ${OPENSSL_CONFIGURE_TARGET}
        --prefix=${OPENSSL_INSTALL_PREFIX}
        --openssldir=${OPENSSL_INSTALL_PREFIX}/ssl
        no-shared no-tests no-ui-console no-docs
    )
    set(_OPENSSL_BUILD_CMD nmake)
    set(_OPENSSL_INSTALL_CMD nmake install_sw)
    set(_SSL_LIB_NAME "libssl.lib")
    set(_CRYPTO_LIB_NAME "libcrypto.lib")
else()
    set(_OPENSSL_CONFIGURE_CMD
        ${CMAKE_COMMAND} -E env
        "CC=${CMAKE_C_COMPILER}"
        ./Configure ${OPENSSL_CONFIGURE_TARGET}
        --prefix=/usr
        --openssldir=/etc/ssl
        --libdir=lib
        no-shared no-tests no-ui-console no-docs
    )
    set(_OPENSSL_BUILD_CMD make -j${NPROC})
    set(_OPENSSL_INSTALL_CMD make DESTDIR=${OPENSSL_STAGE_ROOT} install_sw)
    set(_SSL_LIB_NAME "libssl.a")
    set(_CRYPTO_LIB_NAME "libcrypto.a")
endif()

# ---------------------------------------------------------------------------
# Static library paths (where they'll end up after install_sw)
# ---------------------------------------------------------------------------
set(OPENSSL_SSL_LIBRARY    "${OPENSSL_INSTALL_PREFIX}/lib/${_SSL_LIB_NAME}")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_INSTALL_PREFIX}/lib/${_CRYPTO_LIB_NAME}")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INSTALL_PREFIX}/include")

# ---------------------------------------------------------------------------
# ExternalProject_Add
# ---------------------------------------------------------------------------
ExternalProject_Add(openssl_external
    URL               ${OPENSSL_URL}
    URL_HASH          SHA256=2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_DIR      "${CMAKE_BINARY_DIR}/_download/openssl"
    SOURCE_DIR        "${CMAKE_BINARY_DIR}/_deps/openssl-src"
    BINARY_DIR        "${CMAKE_BINARY_DIR}/_deps/openssl-src"   # OpenSSL builds in-source
    INSTALL_DIR       "${OPENSSL_INSTALL_PREFIX}"
    CONFIGURE_COMMAND ${_OPENSSL_CONFIGURE_CMD}
    BUILD_COMMAND     ${_OPENSSL_BUILD_CMD}
    INSTALL_COMMAND   ${_OPENSSL_INSTALL_CMD}
    BUILD_BYPRODUCTS  ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY}
    LOG_DOWNLOAD      TRUE
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

# ---------------------------------------------------------------------------
# IMPORTED targets that mimic find_package(OpenSSL)
# ---------------------------------------------------------------------------

# -- OpenSSL::Crypto --------------------------------------------------------
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION             "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)
add_dependencies(OpenSSL::Crypto openssl_external)

# -- OpenSSL::SSL -----------------------------------------------------------
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION             "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
)
# SSL depends on Crypto
set_target_properties(OpenSSL::SSL PROPERTIES
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
)
add_dependencies(OpenSSL::SSL openssl_external)

# Platform link dependencies
if(APPLE)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "-framework Security" "-framework CoreFoundation")
elseif(UNIX)
    find_package(Threads REQUIRED)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
elseif(WIN32)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
endif()

# ---------------------------------------------------------------------------
# Set variables so that find_package(OpenSSL) in downstream CMakeLists.txt
# finds our built version instead of Homebrew / system OpenSSL.
# ---------------------------------------------------------------------------
set(OPENSSL_ROOT_DIR    "${OPENSSL_INSTALL_PREFIX}" CACHE PATH   "OpenSSL root (built from source)" FORCE)
set(OPENSSL_INCLUDE_DIR "${OPENSSL_INCLUDE_DIR}"    CACHE PATH   "OpenSSL include directory"        FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "OpenSSL crypto library"     FORCE)
set(OPENSSL_SSL_LIBRARY    "${OPENSSL_SSL_LIBRARY}"    CACHE FILEPATH "OpenSSL SSL library"        FORCE)
set(OPENSSL_FOUND       TRUE                        CACHE BOOL   "OpenSSL found"                   FORCE)
set(OPENSSL_VERSION     "${OPENSSL_VERSION}"         CACHE STRING "OpenSSL version"                 FORCE)

# Make the include directory exist at configure time so CMake doesn't
# complain about INTERFACE_INCLUDE_DIRECTORIES referencing a non-existent path.
file(MAKE_DIRECTORY "${OPENSSL_INCLUDE_DIR}")

message(STATUS "OpenSSL ${OPENSSL_VERSION} will be built from source → ${OPENSSL_INSTALL_PREFIX}")
