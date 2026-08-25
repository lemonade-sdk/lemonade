# ============================================================
# Self-describing backends registry codegen
# ============================================================
# Consumes LEMON_BACKENDS (defined in cmake/Backends.cmake): the foreach below
# compiles each backend's server source and regenerates the registry headers that
# bind every descriptor to its create().
set(LEMON_DESCRIPTOR_INCLUDES "")
set(LEMON_DESCRIPTOR_ENTRIES "")
set(LEMON_FACTORY_INCLUDES "")
set(LEMON_FACTORY_ENTRIES "")
set(LEMON_CAPABILITY_ENTRIES "")
# The data registry (descriptors, header-only) links into both binaries; the
# factory registry + per-backend server sources are server-only.
# Absolute paths so the CLI subdirectory can reuse LEMON_BACKEND_DESCRIPTOR_SOURCES.
set(LEMON_BACKEND_DESCRIPTOR_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/backend_descriptor_registry.cpp)
set(LEMON_BACKEND_FACTORY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/backend_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/backend_ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/hf_cache_util.cpp)
foreach(_backend_entry ${LEMON_BACKENDS})
    string(REPLACE "|" ";" _backend_parts "${_backend_entry}")
    list(GET _backend_parts 1 _backend_stem)
    # The descriptor is header-only (no source). Compile every .cpp in the
    # backend's folder (server class + any backend-private helpers like GGUF
    # parsing) — CONFIGURE_DEPENDS re-globs when a file is added/removed so a new
    # helper in a folder needs no CMake edit. (The backend LIST is still explicit
    # above so a whole new backend is never silently missed.)
    file(GLOB _backend_srcs CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/${_backend_stem}/*.cpp)
    list(APPEND LEMON_BACKEND_FACTORY_SOURCES ${_backend_srcs})
    string(APPEND LEMON_DESCRIPTOR_INCLUDES
        "#include \"lemon/backends/${_backend_stem}/${_backend_stem}.h\"\n")
    string(APPEND LEMON_DESCRIPTOR_ENTRIES
        "        &lemon::backends::${_backend_stem}::descriptor,\n")
    string(APPEND LEMON_FACTORY_INCLUDES
        "#include \"lemon/backends/${_backend_stem}/${_backend_stem}_server.h\"\n")
    string(APPEND LEMON_FACTORY_ENTRIES
        "        { &lemon::backends::${_backend_stem}::descriptor, &lemon::backends::${_backend_stem}::create, lemon::backends::${_backend_stem}::spec(), lemon::backends::${_backend_stem}::ops() },\n")
    string(APPEND LEMON_CAPABILITY_ENTRIES
        "        { &lemon::backends::${_backend_stem}::descriptor, lemon::backends::${_backend_stem}::capabilities() },\n")
endforeach()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/backend_descriptors_generated.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/backend_descriptors_generated.h
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/backend_factories_generated.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/backend_factories_generated.h
    @ONLY)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/backend_capabilities_generated.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/backend_capabilities_generated.h
    @ONLY)

# lemond gets both descriptor data and factories; the CLI gets only the data
# (see src/cpp/cli/CMakeLists.txt, which reuses LEMON_BACKEND_DESCRIPTOR_SOURCES).
list(APPEND SOURCES_CORE ${LEMON_BACKEND_DESCRIPTOR_SOURCES} ${LEMON_BACKEND_FACTORY_SOURCES})
