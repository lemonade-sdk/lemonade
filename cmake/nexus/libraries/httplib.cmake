# Distribution builds use the system package; otherwise the vendored Nexus module
# fetches the pinned source.
# Distribution packages (Debian, Ubuntu) build cpp-httplib as a library whose header
# holds only declarations, so the library has to be linked as well.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT APPLE AND NOT WIN32)
    pkg_check_modules(NEXUS_HTTPLIB QUIET IMPORTED_TARGET cpp-httplib>=0.18.3)
    if(TARGET PkgConfig::NEXUS_HTTPLIB)
        add_library(nexus-httplib INTERFACE)
        add_library(httplib::httplib ALIAS nexus-httplib)
        target_link_libraries(nexus-httplib INTERFACE PkgConfig::NEXUS_HTTPLIB OpenSSL::SSL OpenSSL::Crypto)
        return()
    endif()
endif()
find_path(NEXUS_HTTPLIB_INCLUDE_DIR httplib.h)
if(NEXUS_HTTPLIB_INCLUDE_DIR)
    file(STRINGS "${NEXUS_HTTPLIB_INCLUDE_DIR}/httplib.h" _version REGEX "^#define CPPHTTPLIB_VERSION ")
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _version "${_version}")
    if(_version VERSION_GREATER_EQUAL "0.18.3")
        add_library(nexus-httplib INTERFACE)
        add_library(httplib::httplib ALIAS nexus-httplib)
        target_include_directories(nexus-httplib INTERFACE "${NEXUS_HTTPLIB_INCLUDE_DIR}")
        target_compile_definitions(nexus-httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(nexus-httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        return()
    endif()
endif()

include("${NEXUS_SDK_ROOT}/cmake/libraries/httplib.cmake")
