# The SDK builds against cpp-httplib 0.26.0 or newer,
# the same floor as lemond (Ubuntu ships 0.26.0, Debian 13 backports 0.41.0,
# Fedora 0.48.0). Its event socket needs the WebSocket client's hostname map of
# 0.47.0; below that the SDK keeps the peer list long-poll
# (LemonadeNexusClient::event_socket_supported).
set(_nexus_httplib_min "0.26.0")
# The SDK selects the cpp-httplib API by LEMONADE_NEXUS_HTTPLIB_VERSION
# (major * 100000 + minor * 1000 + patch), as the Nexus module defines it for
# the pinned source; the library's own version macros are strings.
function(_nexus_httplib_version_definition target version)
    if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        message(FATAL_ERROR "cpp-httplib: version '${version}' is not major.minor.patch")
    endif()
    math(EXPR _number "${CMAKE_MATCH_1} * 100000 + ${CMAKE_MATCH_2} * 1000 + ${CMAKE_MATCH_3}")
    target_compile_definitions(${target} INTERFACE LEMONADE_NEXUS_HTTPLIB_VERSION=${_number})
endfunction()
# Distribution packages (Debian, Ubuntu) build cpp-httplib as a library whose header
# holds only declarations, so the library has to be linked as well.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT APPLE AND NOT WIN32)
    pkg_check_modules(NEXUS_HTTPLIB QUIET IMPORTED_TARGET cpp-httplib>=${_nexus_httplib_min})
    # The SDK's HTTPS listener (MeshPort) needs the library's TLS support. A compiled package
    # (Debian, Ubuntu) carries its TLS define in Cflags. A header-only package (Arch) carries
    # none, so the define is added here, as for a bare header below. A compiled package built
    # without TLS cannot serve HTTPS, so the pinned source is used instead.
    set(_nexus_httplib_tls FALSE)
    if(NEXUS_HTTPLIB_CFLAGS_OTHER MATCHES "CPPHTTPLIB_(OPENSSL|MBEDTLS|WOLFSSL)_SUPPORT")
        set(_nexus_httplib_tls TRUE)
    endif()
    if(TARGET PkgConfig::NEXUS_HTTPLIB AND NOT _nexus_httplib_tls AND NEXUS_HTTPLIB_LIBRARIES)
        message(STATUS "Nexus SDK: system cpp-httplib ${NEXUS_HTTPLIB_VERSION} is built without TLS; using the pinned source")
    elseif(TARGET PkgConfig::NEXUS_HTTPLIB)
        add_library(nexus-httplib INTERFACE)
        add_library(httplib::httplib ALIAS nexus-httplib)
        target_link_libraries(nexus-httplib INTERFACE PkgConfig::NEXUS_HTTPLIB OpenSSL::SSL OpenSSL::Crypto)
        if(NOT _nexus_httplib_tls)
            target_compile_definitions(nexus-httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        endif()
        _nexus_httplib_version_definition(nexus-httplib "${NEXUS_HTTPLIB_VERSION}")
        message(STATUS "Nexus SDK: system cpp-httplib ${NEXUS_HTTPLIB_VERSION}")
        return()
    endif()
endif()
find_path(NEXUS_HTTPLIB_INCLUDE_DIR httplib.h)
if(NEXUS_HTTPLIB_INCLUDE_DIR)
    file(STRINGS "${NEXUS_HTTPLIB_INCLUDE_DIR}/httplib.h" _version REGEX "^#define CPPHTTPLIB_VERSION ")
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _version "${_version}")
    if(_version VERSION_GREATER_EQUAL "${_nexus_httplib_min}")
        add_library(nexus-httplib INTERFACE)
        add_library(httplib::httplib ALIAS nexus-httplib)
        target_include_directories(nexus-httplib INTERFACE "${NEXUS_HTTPLIB_INCLUDE_DIR}")
        target_compile_definitions(nexus-httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(nexus-httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        _nexus_httplib_version_definition(nexus-httplib "${_version}")
        message(STATUS "Nexus SDK: system cpp-httplib header ${_version}")
        return()
    endif()
endif()

include("${NEXUS_SDK_ROOT}/cmake/libraries/httplib.cmake")
