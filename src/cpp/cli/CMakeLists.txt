# CLI client subproject
# Note: This is included from parent CMakeLists.txt via add_subdirectory(cli)
# Parent project already sets C++ standard and fetches dependencies


# ============================================================
# Download digest verification dependency
# ============================================================
# http_client.cpp is compiled into both lemonade-server-core and the lemonade
# CLI.  Digest verification must therefore be wired to both targets from here.
# Prefer a distro-provided mbedcrypto package, but fall back to FetchContent so
# local Lemonade builds do not require developers to install a new package.
if(TARGET lemonade-server-core)
    target_link_libraries(lemonade-server-core PUBLIC lemonade-digest-crypto)
endif()

# Common sources
set(COMMON_SOURCES
    main.cpp
    lemonade_client.cpp
    model_selection.cpp
    recipe_import.cpp
    hf_pull.cpp
    bench.cpp
    bench_output.cpp
    bench_comparison.cpp
    bench_sysinfo.cpp
    chat_repl.cpp
    ../server/recipe_options.cpp
    ../server/utils/process_manager.cpp
    ../server/utils/path_utils.cpp
    ../server/utils/url_utils.cpp
    ../server/utils/json_utils.cpp
    ../server/utils/http_client.cpp
    # recipe_import.cpp persists and validates remote registry provenance.
    # Compile the provider implementation into the standalone CLI as well; the
    # CLI does not link lemonade-server-core, where this source is otherwise built.
    ../server/model_registry.cpp
    agent_launcher.cpp
    agent_config_file.cpp
    opencode_profile.cpp
    pi_profile.cpp
    # Self-describing backend descriptors (plain data; CLI-safe). Lets the CLI
    # read recipe options/flags from descriptors without linking server classes.
    # The matching factories (create()) are server-only and NOT listed here.
    ${LEMON_BACKEND_DESCRIPTOR_SOURCES}
)

# Add platform-specific sources
if(WIN32)
    list(APPEND COMMON_SOURCES
        ../server/utils/platform/path_windows.cpp
        ../server/utils/platform/process_windows.cpp
    )
elseif(APPLE)
    list(APPEND COMMON_SOURCES
        ../server/utils/platform/path_macos.cpp
        ../server/utils/platform/process_macos.cpp
    )
elseif(UNIX)
    list(APPEND COMMON_SOURCES
        ../server/utils/platform/path_linux.cpp
        ../server/utils/platform/process_linux.cpp
    )
endif()

# ============================================================
# lemonade - HTTP client CLI
# ============================================================
add_executable(lemonade
    ${COMMON_SOURCES}
)

target_link_libraries(lemonade PRIVATE
    nlohmann_json::nlohmann_json
    lemonade-digest-crypto
    lemonade-httplib
)

# Link CLI11 based on what's available
if(USE_SYSTEM_CLI11)
    target_include_directories(lemonade PRIVATE ${CLI11_INCLUDE_DIRS})
else()
    target_link_libraries(lemonade PRIVATE CLI11::CLI11)
endif()

# Link curl based on what's available
if(USE_SYSTEM_CURL)
    target_link_libraries(lemonade PRIVATE ${CURL_LIBRARIES})
    target_include_directories(lemonade PRIVATE ${CURL_INCLUDE_DIRS})
    target_compile_options(lemonade PRIVATE ${CURL_CFLAGS_OTHER})
else()
    target_link_libraries(lemonade PRIVATE libcurl)
endif()

# Link zstd based on what's available
if(USE_SYSTEM_ZSTD)
    target_link_libraries(lemonade PRIVATE ${ZSTD_LIBRARIES})
    target_include_directories(lemonade PRIVATE ${ZSTD_INCLUDE_DIRS})
    target_link_directories(lemonade PRIVATE ${ZSTD_LIBRARY_DIRS})
    target_compile_options(lemonade PRIVATE ${ZSTD_CFLAGS_OTHER})
endif()

# Add include directories for header-only system packages
target_include_directories(lemonade PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

# Set output directory to build root
set_target_properties(lemonade PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    set_target_properties(lemonade PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

target_compile_definitions(lemonade PRIVATE LEMONADE_CLI)

# Platform-specific settings
if(WIN32)
    # On Windows, make sure we have Unicode support
    target_compile_definitions(lemonade PRIVATE
        UNICODE
        _UNICODE
    )

    # Link with networking and system libraries
    target_link_libraries(lemonade PRIVATE
        ws2_32
        wsock32
    )

    # Embed manifest file if MSVC
    if(MSVC)
        # Note: lemonade does not use a manifest file
        set_target_properties(lemonade PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED"
        )
    endif()
endif()

if(APPLE)
    # Enable ARC (Automatic Reference Counting) for macOS Objective-C++ files
    target_compile_options(lemonade PRIVATE -fobjc-arc)

    target_link_libraries(lemonade PRIVATE
        "-framework Foundation"
        "-framework CoreServices"
    )
endif()

if(UNIX AND NOT APPLE)
    # On Linux, link with pthread
    find_package(Threads REQUIRED)
    target_link_libraries(lemonade PRIVATE
        Threads::Threads
    )
endif()

# Install target (only on non-macOS platforms where main CMakeLists doesn't handle it)
if(NOT APPLE)
    install(TARGETS lemonade
        RUNTIME DESTINATION bin
    )
endif()

# ============================================================
# macOS Signing (Release builds only)
# ============================================================
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Configuring signing for CLI Application (lemonade)")

    # Inherit variables from Parent CMake
    if(NOT DEFINED SIGNING_IDENTITY OR NOT DEFINED ENTITLEMENTS_PATH)
        message(FATAL_ERROR "Signing variables missing! Ensure add_subdirectory(cli) is called AFTER identities are set in root CMakeLists.txt")
    endif()

    add_custom_command(TARGET lemonade POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "--- Signing lemonade (CLI) ---"
        COMMAND codesign --force --options runtime --timestamp --entitlements "${ENTITLEMENTS_PATH}" --sign "${SIGNING_IDENTITY}" -v $<TARGET_FILE:lemonade>
        COMMENT "Signing lemonade with Hardened Runtime"
        VERBATIM
    )
endif()

# Create symlink in standard bin path only if not installing to /usr
if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_INSTALL_PREFIX}/bin/lemonade
                \"\$ENV{DESTDIR}/usr/bin/lemonade\"
        )
    ")
endif()
