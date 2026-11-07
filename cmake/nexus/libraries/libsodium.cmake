# Distribution builds use the system package; otherwise the vendored Nexus module
# fetches the pinned source.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT APPLE AND NOT WIN32)
    pkg_check_modules(NEXUS_SODIUM QUIET IMPORTED_TARGET libsodium>=1.0.18)
    if(TARGET PkgConfig::NEXUS_SODIUM)
        add_library(sodium INTERFACE)
        target_link_libraries(sodium INTERFACE PkgConfig::NEXUS_SODIUM)
        return()
    endif()
endif()

include("${NEXUS_SDK_ROOT}/cmake/libraries/libsodium.cmake")
