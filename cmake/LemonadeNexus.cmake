option(LEMONADE_BUNDLE_NEXUS "Build and package the Lemonade Nexus sidecar" OFF)
set(LEMONADE_NEXUS_SOURCE_DIR "" CACHE PATH "Use a local lemonade-nexus checkout instead of fetching it")
set(LEMONADE_NEXUS_GIT_TAG
    "427dc33841f4a2a21fbdc5b6d57325b682a66673"
    CACHE STRING "Pinned lemonade-nexus commit")

if(NOT LEMONADE_BUNDLE_NEXUS)
    return()
endif()

if(CMAKE_VERSION VERSION_LESS 3.25.1)
    message(FATAL_ERROR "LEMONADE_BUNDLE_NEXUS requires CMake 3.25.1 or newer")
endif()

include(ExternalProject)
include(FetchContent)

if(LEMONADE_NEXUS_SOURCE_DIR)
    get_filename_component(_lemonade_nexus_source_dir
        "${LEMONADE_NEXUS_SOURCE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${_lemonade_nexus_source_dir}/projects/LemonadeNexusSidecar/CMakeLists.txt")
        message(FATAL_ERROR
            "LEMONADE_NEXUS_SOURCE_DIR is not a bundling-capable lemonade-nexus checkout: "
            "${_lemonade_nexus_source_dir}")
    endif()
else()
    FetchContent_Declare(lemonade_nexus_source
        GIT_REPOSITORY https://github.com/lemonade-sdk/lemonade-nexus.git
        GIT_TAG "${LEMONADE_NEXUS_GIT_TAG}"
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
        SOURCE_SUBDIR cmake/fetch-only
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(lemonade_nexus_source)
    set(_lemonade_nexus_source_dir "${lemonade_nexus_source_SOURCE_DIR}")
endif()

set(_lemonade_nexus_binary_dir
    "${CMAKE_BINARY_DIR}/_deps/lemonade-nexus-build")

if(WIN32)
    set(_lemonade_nexus_executable_suffix ".exe")
else()
    set(_lemonade_nexus_executable_suffix "")
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(_lemonade_nexus_sidecar
        "${_lemonade_nexus_binary_dir}/projects/LemonadeNexusSidecar/$<CONFIG>/lemonade-nexus-sidecar${_lemonade_nexus_executable_suffix}")
else()
    set(_lemonade_nexus_sidecar
        "${_lemonade_nexus_binary_dir}/projects/LemonadeNexusSidecar/lemonade-nexus-sidecar${_lemonade_nexus_executable_suffix}")
endif()

set(_lemonade_nexus_cmake_args
    "-DBUILD_TESTING:BOOL=OFF"
    "-DLEMONADE_NEXUS_MINIMAL_DEPS:BOOL=ON"
)

if(NOT LEMONADE_NEXUS_SOURCE_DIR)
    list(APPEND _lemonade_nexus_cmake_args
        "-DNEXUS_GIT_COMMIT_OVERRIDE:STRING=${LEMONADE_NEXUS_GIT_TAG}")
endif()
if(CMAKE_BUILD_TYPE)
    list(APPEND _lemonade_nexus_cmake_args
        "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}")
endif()
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _lemonade_nexus_cmake_args
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
endif()
if(VCPKG_TARGET_TRIPLET)
    list(APPEND _lemonade_nexus_cmake_args
        "-DVCPKG_TARGET_TRIPLET:STRING=${VCPKG_TARGET_TRIPLET}")
endif()
if(OPENSSL_ROOT_DIR)
    list(APPEND _lemonade_nexus_cmake_args
        "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
        "-DOPENSSL_USE_STATIC_LIBS:BOOL=ON")
endif()
if(CMAKE_OSX_ARCHITECTURES)
    string(REPLACE ";" "|" _lemonade_nexus_osx_architectures
        "${CMAKE_OSX_ARCHITECTURES}")
    list(APPEND _lemonade_nexus_cmake_args
        "-DCMAKE_OSX_ARCHITECTURES:STRING=${_lemonade_nexus_osx_architectures}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _lemonade_nexus_cmake_args
        "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if(CMAKE_OSX_SYSROOT)
    list(APPEND _lemonade_nexus_cmake_args
        "-DCMAKE_OSX_SYSROOT:PATH=${CMAKE_OSX_SYSROOT}")
endif()
if(APPLE)
    list(APPEND _lemonade_nexus_cmake_args
        "-DOPENSSL_FORCE_BUNDLED:BOOL=ON")
endif()

# Keep Nexus in an isolated CMake build because both projects currently declare
# overlapping third-party targets and cache variables.
ExternalProject_Add(lemonade_nexus_external
    SOURCE_DIR "${_lemonade_nexus_source_dir}"
    BINARY_DIR "${_lemonade_nexus_binary_dir}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    LIST_SEPARATOR "|"
    CMAKE_ARGS ${_lemonade_nexus_cmake_args}
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build <BINARY_DIR>
        --config $<CONFIG>
        --target LemonadeNexusSidecar
        --parallel
    BUILD_BYPRODUCTS "${_lemonade_nexus_sidecar}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
)

install(PROGRAMS "${_lemonade_nexus_sidecar}"
    DESTINATION bin
    COMPONENT Runtime)

function(_lemonade_nexus_finalize_targets)
    foreach(_package_target
            package
            wix_installers
            wix_installer_minimal
            wix_installer_full
            package-macos)
        if(TARGET ${_package_target})
            add_dependencies(${_package_target} lemonade_nexus_external)
        endif()
    endforeach()
endfunction()

cmake_language(DEFER CALL _lemonade_nexus_finalize_targets)

message(STATUS
    "Lemonade Nexus sidecar enabled from ${_lemonade_nexus_source_dir}")
