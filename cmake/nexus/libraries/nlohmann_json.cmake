# Distribution builds use the system package; otherwise the vendored Nexus module
# fetches the pinned source.
if(NOT APPLE AND NOT WIN32)
    find_package(nlohmann_json QUIET)
    if(TARGET nlohmann_json::nlohmann_json)
        return()
    endif()
endif()

include("${NEXUS_SDK_ROOT}/cmake/libraries/nlohmann_json.cmake")
