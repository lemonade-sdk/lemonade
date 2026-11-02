include(FetchContent)

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
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# The non-blocking getaddrinfo uses CFHost on every Apple platform, but
# cpp-httplib links CFNetwork only for macOS ($<PLATFORM_ID:Darwin>).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(httplib INTERFACE "-framework CFNetwork" "-framework CoreFoundation")
endif()
