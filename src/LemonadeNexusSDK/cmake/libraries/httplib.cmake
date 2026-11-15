include(FetchContent)

# The server builds this release. The client SDK also builds against the
# cpp-httplib a distribution ships, 0.26.0 or newer (Ubuntu: 0.26.0; Debian 13
# backports: 0.41.0; Fedora: 0.48.0); the event socket needs the WebSocket
# client's hostname map of 0.47.0 and is absent below it (see
# LemonadeNexusClient::event_socket_supported). A downstream build names the
# system library through its own find module; FETCHCONTENT_SOURCE_DIR_HTTPLIB
# names a local copy of a release for an offline or a trial build.
FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.61.0
)

option(HTTPLIB_REQUIRE_OPENSSL "Require OpenSSL for HTTPS" ON)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
# No HTTP compression: Nexus does not use it, and a system zlib or zstd found
# at configure time would become a runtime dependency of every binary that
# links httplib (and breaks the mobile cross builds).
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# The client SDK selects the cpp-httplib API by LEMONADE_NEXUS_HTTPLIB_VERSION
# (major * 100000 + minor * 1000 + patch): the library's own version macros
# are strings. A build that links httplib without the definition fails to
# compile the SDK, so an unknown library cannot drop a feature unnoticed.
file(STRINGS "${httplib_SOURCE_DIR}/httplib.h" _ln_httplib_version_line
     REGEX "^#define CPPHTTPLIB_VERSION ")
if(NOT _ln_httplib_version_line MATCHES "\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"")
    message(FATAL_ERROR "cpp-httplib: no CPPHTTPLIB_VERSION in ${httplib_SOURCE_DIR}/httplib.h")
endif()
math(EXPR _ln_httplib_version
     "${CMAKE_MATCH_1} * 100000 + ${CMAKE_MATCH_2} * 1000 + ${CMAKE_MATCH_3}")
target_compile_definitions(httplib INTERFACE LEMONADE_NEXUS_HTTPLIB_VERSION=${_ln_httplib_version})

# The non-blocking getaddrinfo uses CFHost on every Apple platform, but
# cpp-httplib links CFNetwork only for macOS ($<PLATFORM_ID:Darwin>).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(httplib INTERFACE "-framework CFNetwork" "-framework CoreFoundation")
endif()
