if(NOT APPLE AND NOT WIN32)
    find_package(spdlog QUIET)
    if(TARGET spdlog::spdlog AND NOT TARGET spdlog)
        add_library(spdlog ALIAS spdlog::spdlog)
    endif()
    if(TARGET spdlog)
        return()
    endif()
endif()

include(FetchContent)

FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 486b55554f11c9cccc913e11a87085b2a91f706f  # v1.16.0
)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)
