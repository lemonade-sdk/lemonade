# Finds a Cargo that can build the Nexus SDK's Rust transport and sets
# LEMONADE_CARGO_EXECUTABLE and _nexus_toolchain_found.
#
# The locked crates need Rust 1.85: zeroize 1.9 uses edition 2024, and the
# lockfiles use format version 4. Ubuntu 24.04 ships Cargo 1.75 as `cargo`
# and newer toolchains as versioned packages (cargo-1.85 installs
# /usr/lib/rust-1.85/bin/cargo), so those directories are searched after PATH.
set(_nexus_minimum_cargo 1.85)

function(_nexus_cargo_version cargo out)
    set(${out} "" PARENT_SCOPE)
    execute_process(COMMAND "${cargo}" --version
        OUTPUT_VARIABLE _output ERROR_QUIET RESULT_VARIABLE _result)
    if(_result EQUAL 0)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _version "${_output}")
        set(${out} "${_version}" PARENT_SCOPE)
    endif()
endfunction()

set(_nexus_cargo_candidates)
if(LEMONADE_CARGO_EXECUTABLE)
    list(APPEND _nexus_cargo_candidates "${LEMONADE_CARGO_EXECUTABLE}")
else()
    find_program(_nexus_cargo_in_path cargo NO_CACHE)
    if(_nexus_cargo_in_path)
        list(APPEND _nexus_cargo_candidates "${_nexus_cargo_in_path}")
    endif()
    file(GLOB _nexus_versioned_cargo "/usr/lib/rust-1.*/bin/cargo")
    list(SORT _nexus_versioned_cargo COMPARE NATURAL ORDER DESCENDING)
    list(APPEND _nexus_cargo_candidates ${_nexus_versioned_cargo})
endif()

set(_nexus_toolchain_found OFF)
foreach(_cargo IN LISTS _nexus_cargo_candidates)
    _nexus_cargo_version("${_cargo}" _version)
    if(_version AND _version VERSION_GREATER_EQUAL _nexus_minimum_cargo)
        set(LEMONADE_CARGO_EXECUTABLE "${_cargo}" CACHE FILEPATH "Cargo used to build the Nexus SDK transport" FORCE)
        set(_nexus_toolchain_found ON)
        message(STATUS "Nexus Rust toolchain: ${_cargo} (${_version})")
        break()
    endif()
endforeach()
