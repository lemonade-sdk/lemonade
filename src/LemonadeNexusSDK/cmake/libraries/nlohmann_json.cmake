if(NOT APPLE AND NOT WIN32)
    find_package(nlohmann_json QUIET)
    if(TARGET nlohmann_json::nlohmann_json)
        return()
    endif()
endif()

include(FetchContent)

FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 55f93686c01528224f448c19128836e7df245f72  # v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)
