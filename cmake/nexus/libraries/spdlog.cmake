# Distribution builds use the system package; otherwise the vendored Nexus module
# fetches the pinned source.
if(NOT APPLE AND NOT WIN32)
    find_package(spdlog QUIET)
    if(TARGET spdlog::spdlog AND NOT TARGET spdlog)
        add_library(spdlog ALIAS spdlog::spdlog)
    endif()
    if(TARGET spdlog)
        return()
    endif()
endif()

include("${NEXUS_SDK_ROOT}/cmake/libraries/spdlog.cmake")
