cmake_minimum_required(VERSION 3.12...3.28)

# Use static runtime library on Windows to avoid DLL dependencies
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(lemon_cpp VERSION 11.7.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LemonadeTesting.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildConfig.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# ============================================================
# Application: lemond
# ============================================================

set(EXECUTABLE_NAME "lemond")


# ============================================================
# Compilation configurations
# ============================================================

# Common compiler definitions
# Enable httplib thread pool with 8 threads
add_compile_definitions(CPPHTTPLIB_THREAD_POOL_COUNT=8)

# Platform-specific compiler definitions
if(WIN32)
    # Set Windows target version to Windows 10 for httplib v0.26.0, and prevent Windows.h min/max macro definitions
    add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX)
    if(MSVC)
        # Add security-hardening compiler flags for MSVC
        # Control Flow Guard - prevents control flow hijacking
        add_compile_options(/guard:cf)
        # Buffer Security Check - stack buffer overflow detection
        add_compile_options(/GS)
        # MSBuild parallelizes across projects, but within one project cl.exe
        # compiles a single file at a time, which leaves most cores idle.
        # Ninja already schedules per file, so /MP would oversubscribe there.
        # Applies to this project's targets only — the FetchContent
        # dependencies above were already configured by this point.
        # Do not pass --parallel to `cmake --build` with a Visual Studio
        # generator: CMake then sets CL_MPCount=1, which disables this.
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            add_compile_options(/MP)
        endif()
    endif()
endif()


# ============================================================
# Compilation
# ============================================================

# Generate version header from template
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include/lemon/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lemon/version.h
    @ONLY
)

# Generate manifest files from templates (Windows only)
if(WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/lemonade.manifest.in
        ${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest
        @ONLY
    )

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackagingWindows.cmake)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# ============================================================
# Server core sources (everything except entry points)
# ============================================================
set(SOURCES_CORE
    src/cpp/server/server.cpp
    src/cpp/server/collection_orchestrator.cpp
    src/cpp/server/jobs/job_ops.cpp
    src/cpp/server/jobs/job_manager.cpp
    src/cpp/server/router.cpp
    src/cpp/server/global_vram_monitor.cpp
    src/cpp/server/eviction_engine.cpp
    src/cpp/server/cli_parser.cpp
    src/cpp/server/cloud_provider_registry.cpp
    src/cpp/server/config_file.cpp
    src/cpp/server/directory_watcher.cpp
    src/cpp/server/alias_manager.cpp
    src/cpp/server/model_manager.cpp
    src/cpp/server/model_registry.cpp
    src/cpp/server/hf_variants.cpp
    src/cpp/server/wrapped_server.cpp
    src/cpp/server/streaming_proxy.cpp
    src/cpp/server/system_info.cpp
    src/cpp/server/recipe_options.cpp
    src/cpp/server/routing_classifier_services.cpp
    src/cpp/server/routing_classifier_services_router.cpp
    src/cpp/server/thinking_controls.cpp
    src/cpp/server/routing_policy.cpp
    src/cpp/server/routing_policy_parser.cpp
    src/cpp/server/routing_policy_store.cpp
    src/cpp/server/route_decision_response.cpp
    src/cpp/server/runtime_config.cpp
    src/cpp/server/telemetry.cpp
    src/cpp/server/logging_config.cpp
    src/cpp/server/log_stream.cpp
    src/cpp/server/prometheus_metrics.cpp
    src/cpp/server/utils/http_client.cpp
    src/cpp/server/utils/json_utils.cpp
    src/cpp/server/utils/process_manager.cpp
    src/cpp/server/utils/path_utils.cpp
    src/cpp/server/utils/url_utils.cpp
    src/cpp/server/utils/version_utils.cpp
    src/cpp/server/utils/wmi_helper.cpp
    src/cpp/server/utils/network_beacon.cpp
    src/cpp/server/utils/tcp_jsonl_client.cpp
    src/cpp/server/backends/backend_utils.cpp
    src/cpp/server/backend_manager.cpp
    src/cpp/server/ollama_api.cpp
    src/cpp/server/anthropic_api.cpp
    src/cpp/server/mcp_client.cpp
    src/cpp/server/mcp_server.cpp
    src/cpp/server/streaming_audio_buffer.cpp
    src/cpp/server/vad.cpp
    src/cpp/server/realtime_session.cpp
    src/cpp/server/websocket_server.cpp
)

# Add platform-specific source files to core
if(WIN32)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/path_windows.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/platform/metrics_windows.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/platform/suspend_stub.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/archive_windows.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/process_windows.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/network_windows.cpp)
elseif(APPLE)
    list(APPEND SOURCES_CORE src/cpp/server/macos_system_info.mm)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/path_macos.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/platform/metrics_macos.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/platform/suspend_stub.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/archive_unix.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/process_macos.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/network_unix.cpp)
    set_source_files_properties(src/cpp/server/macos_system_info.mm PROPERTIES LANGUAGE OBJCXX)
elseif(UNIX)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/path_linux.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/platform/metrics_linux.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/platform/suspend_linux.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/archive_unix.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/process_linux.cpp)
    list(APPEND SOURCES_CORE src/cpp/server/utils/platform/network_unix.cpp)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BackendCodegen.cmake)

# ============================================================
# Server core OBJECT library (shared by lemond and Lemonade.exe)
# ============================================================
add_library(lemonade-server-core OBJECT ${SOURCES_CORE})

# ============================================================
# lemond executable
# ============================================================
add_executable(${EXECUTABLE_NAME} src/cpp/server/main.cpp)
if(WIN32)
    target_sources(${EXECUTABLE_NAME} PRIVATE src/cpp/server/version.rc)
endif()


# ============================================================
# Linking configurations
# ============================================================

# Platform-specific linking options
if(WIN32)
    if(MSVC)
        # Linker security flags
        add_link_options(
            /DYNAMICBASE      # Address Space Layout Randomization (ASLR)
            /NXCOMPAT         # Data Execution Prevention (DEP)
            /GUARD:CF         # Control Flow Guard
        )
        # Embed manifest file. /MANIFESTINPUT's path must be quoted; unquoted,
        # a path containing a space breaks the linker with LNK1181.
        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:\"${CMAKE_CURRENT_BINARY_DIR}/server/lemonade.manifest\""
        )
    endif()
endif()


# ============================================================
# Server core dependencies (PUBLIC — inherited by consumers)
# ============================================================

target_link_libraries(lemonade-server-core PUBLIC
    nlohmann_json::nlohmann_json
    lemonade-httplib
)

if(USE_SYSTEM_CLI11)
    target_include_directories(lemonade-server-core PUBLIC ${CLI11_INCLUDE_DIRS})
else()
    target_link_libraries(lemonade-server-core PUBLIC CLI11::CLI11)
endif()

if(USE_SYSTEM_CURL)
    target_link_libraries(lemonade-server-core PUBLIC ${CURL_LIBRARIES})
    target_include_directories(lemonade-server-core PUBLIC ${CURL_INCLUDE_DIRS})
    target_compile_options(lemonade-server-core PUBLIC ${CURL_CFLAGS_OTHER})
else()
    target_link_libraries(lemonade-server-core PUBLIC libcurl)
endif()

if(USE_SYSTEM_ZSTD)
    target_link_libraries(lemonade-server-core PUBLIC ${ZSTD_LIBRARIES})
    target_include_directories(lemonade-server-core PUBLIC ${ZSTD_INCLUDE_DIRS})
    target_link_directories(lemonade-server-core PUBLIC ${ZSTD_LIBRARY_DIRS})
    target_compile_options(lemonade-server-core PUBLIC ${ZSTD_CFLAGS_OTHER})
endif()

lemonade_enable_test_registration_guard()

# libwebsockets for the realtime and log-stream WebSocket server
if(USE_SYSTEM_LWS)
    target_link_libraries(lemonade-server-core PUBLIC ${LWS_LIBRARIES})
    target_include_directories(lemonade-server-core PUBLIC ${LWS_INCLUDE_DIRS})
    target_link_directories(lemonade-server-core PUBLIC ${LWS_LIBRARY_DIRS})
    target_compile_options(lemonade-server-core PUBLIC ${LWS_CFLAGS_OTHER})
else()
    target_link_libraries(lemonade-server-core PUBLIC websockets)
    target_include_directories(lemonade-server-core PUBLIC ${libwebsockets_BINARY_DIR}/include ${libwebsockets_SOURCE_DIR}/include)
endif()

# Enable ARC (Automatic Reference Counting) for macOS Objective-C++ files
if(APPLE)
    target_compile_options(lemonade-server-core PUBLIC -fobjc-arc)
endif()

# Platform-specific linking
if(WIN32)
    target_link_libraries(lemonade-server-core PUBLIC
        ws2_32
        wsock32
        wbemuuid
        ole32
        oleaut32
        shell32
    )
elseif(APPLE)
    target_link_libraries(lemonade-server-core PUBLIC
        "-framework Metal"
        "-framework Foundation"
        "-framework CoreServices"
        "-lobjc"
    )
    target_link_options(lemonade-server-core PUBLIC -Wl,-no_weak_imports)
elseif(UNIX)
    # Link systemd for journal support
    if(PkgConfig_FOUND)
        pkg_check_modules(SYSTEMD QUIET libsystemd)
        if(SYSTEMD_FOUND)
            set(LEMONADE_SYSTEMD_UNIT_NAME "lemond.service" CACHE STRING
                "Systemd unit name to match for journald log mode (overridable at runtime via LEMONADE_SYSTEMD_UNIT env var)")
            target_include_directories(lemonade-server-core PUBLIC ${SYSTEMD_INCLUDE_DIRS})
            target_link_libraries(lemonade-server-core PUBLIC ${SYSTEMD_LIBRARIES})
            target_compile_definitions(lemonade-server-core PUBLIC
                HAVE_SYSTEMD
                LEMONADE_SYSTEMD_UNIT_NAME="${LEMONADE_SYSTEMD_UNIT_NAME}")
        endif()
    endif()

    # Link libcap for capability management on Linux
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(lemonade-server-core PRIVATE
            ${CMAKE_DL_LIBS}
            drm_amdgpu
        )

        if(PkgConfig_FOUND)
            pkg_check_modules(LIBCAP QUIET libcap)
            if(LIBCAP_FOUND)
                target_include_directories(lemonade-server-core PUBLIC ${LIBCAP_INCLUDE_DIRS})
                target_link_libraries(lemonade-server-core PUBLIC ${LIBCAP_LIBRARIES})
                target_compile_definitions(lemonade-server-core PUBLIC HAVE_LIBCAP)
                message(STATUS "libcap found - capability inheritance will be enabled")
            else()
                message(STATUS "libcap not found - capability inheritance will be disabled")
            endif()
        endif()
    endif()
endif()

# lemond inherits all deps from lemonade-server-core
target_link_libraries(${EXECUTABLE_NAME} PRIVATE lemonade-server-core)

# ============================================================
# Resources
# ============================================================

# Resources are self-contained in src/cpp/resources/:
#   - static/index.html, static/favicon.ico (web UI)
#   - server_models.json (model registry)
#   - backend_versions.json (backend version configuration)
#   - defaults.json (default config values)
#   - bench_scenarios.json (benchmark scenarios)

# Collect resource files for dependency tracking
file(GLOB_RECURSE RESOURCE_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/resources/*"
)

# The Omni tool definitions are authored in the frontend tree and are the single
# source of truth (the desktop app imports them directly). Stage a copy into the
# resources tree so the C++ server reads the same file via get_resource_path.
set(TOOL_DEFINITIONS_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/src/renderer/utils/toolDefinitions.json")

# Create a custom target that depends on resource files
# This ensures resources are copied when source files change
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/resources.stamp
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/resources
        ${CMAKE_BINARY_DIR}/resources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${TOOL_DEFINITIONS_SRC}
        ${CMAKE_BINARY_DIR}/resources/toolDefinitions.json
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/resources.stamp
    DEPENDS ${RESOURCE_FILES} ${TOOL_DEFINITIONS_SRC}
    COMMENT "Copying resources to build directory"
)

add_custom_target(copy_resources ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/resources.stamp
)

# Copy resources to the runtime directory after build
add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/resources
        $<TARGET_FILE_DIR:${EXECUTABLE_NAME}>/resources
    COMMENT "Copying resources to output directory"
)

# Ensure resources are copied before building the executable
add_dependencies(${EXECUTABLE_NAME} copy_resources)

# ============================================================
# Tauri App Integration (Cross-Platform)
# ============================================================

# Find Node.js and npm for the renderer bundle, and Cargo for the Rust host.
find_program(NODE_EXECUTABLE node)
# On Windows, npm is a batch file (npm.cmd), so we need to find it explicitly
# or invoke it through cmd.exe to avoid MSBuild custom build step issues
if(WIN32)
    find_program(NPM_EXECUTABLE npm.cmd)
else()
    find_program(NPM_EXECUTABLE npm)
endif()

find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
find_program(RUSTC_EXECUTABLE rustc HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")

# Custom target to build the Tauri app (must be called manually)
if(NODE_EXECUTABLE AND NPM_EXECUTABLE AND CARGO_EXECUTABLE)
    if(WIN32)
        # On Windows, wrap npm calls with cmd /c to handle batch file execution properly
        set(NPM_COMMAND cmd /c "${NPM_EXECUTABLE}")
    else()
        set(NPM_COMMAND "${NPM_EXECUTABLE}")
    endif()

    set(TAURI_APP_OUTPUT "${TAURI_APP_UNPACKED_DIR}/${TAURI_EXE_NAME}")

    file(GLOB_RECURSE TAURI_APP_SOURCES
        CONFIGURE_DEPENDS
        "${TAURI_APP_SOURCE_DIR}/src/*"
        "${TAURI_APP_SOURCE_DIR}/src-tauri/src/*"
        "${TAURI_APP_SOURCE_DIR}/src-tauri/Cargo.toml"
        "${TAURI_APP_SOURCE_DIR}/src-tauri/tauri.conf.json"
        "${TAURI_APP_SOURCE_DIR}/src-tauri/capabilities/*.json"
        "${TAURI_APP_SOURCE_DIR}/assets/*"
        "${TAURI_APP_SOURCE_DIR}/*.json"
        "${TAURI_APP_SOURCE_DIR}/*.js"
        "${TAURI_APP_SOURCE_DIR}/*.ts"
        "${TAURI_APP_SOURCE_DIR}/*.tsx"
        "${TAURI_APP_SOURCE_DIR}/*.css"
        "${TAURI_APP_SOURCE_DIR}/*.html"
    )
    list(FILTER TAURI_APP_SOURCES EXCLUDE REGEX "[/\\\\](node_modules|dist|target)[/\\\\]")

    # On macOS we need the .app bundle for productbuild; on Linux/Windows we
    # just need the bare executable and can skip Tauri's own bundlers.
    # Each platform has a dedicated `npm run build:*` script in src/app/package.json
    # so we can use the same `npm run <script>` invocation pattern as the
    # previous electron-app target (which MSBuild custom-commands handle cleanly —
    # `npm exec --` with cmd /c chaining breaks MSBuild's VCEnd label).
    if(APPLE)
        set(TAURI_BUILD_SCRIPT "build:mac")
        set(TAURI_BUILD_OUTPUT_SOURCE "${TAURI_APP_CARGO_TARGET}/bundle/macos/lemonade-app.app")
        set(TAURI_COPY_COMMAND ${CMAKE_COMMAND} -E copy_directory "${TAURI_BUILD_OUTPUT_SOURCE}" "${TAURI_APP_UNPACKED_DIR}/lemonade-app.app")
    else()
        set(TAURI_BUILD_SCRIPT "build:nobundle")
        if(WIN32)
            set(TAURI_BUILD_OUTPUT_SOURCE "${TAURI_APP_CARGO_TARGET}/lemonade-app.exe")
        else()
            set(TAURI_BUILD_OUTPUT_SOURCE "${TAURI_APP_CARGO_TARGET}/lemonade-app")
        endif()
        set(TAURI_COPY_COMMAND ${CMAKE_COMMAND} -E copy "${TAURI_BUILD_OUTPUT_SOURCE}" "${TAURI_APP_OUTPUT}")
    endif()

    # `tauri build` shells out to `cargo` (e.g. `cargo metadata` at startup),
    # so cargo MUST be on PATH for the npm subprocess. CI sets this on every
    # platform via dtolnay/rust-toolchain@stable. For local developers we
    # also inject CARGO_EXECUTABLE's directory into PATH for the build
    # subprocess, so a developer who has Rust installed but hasn't sourced
    # rustup's env in their current shell still gets a working build.
    #
    # IMPORTANT: this PATH injection is gated on NOT WIN32. On Windows,
    # MSBuild's custom-build cmd-file generator can't handle the nested
    # `cmake -E env ... cmake -E chdir ... cmd /c npm.cmd run ...` shape —
    # it produces a `The system cannot find the batch label specified -
    # VCEnd` error (the same MSBuild quirk that commit 2b515ceb already had
    # to dance around). Windows builds rely on cargo being on the parent
    # shell's PATH, which CI satisfies and local Windows developers get
    # automatically because rustup-init.exe updates the user PATH.
    get_filename_component(TAURI_CARGO_BIN_DIR "${CARGO_EXECUTABLE}" DIRECTORY)
    if(WIN32)
        set(TAURI_BUILD_COMMAND
            ${CMAKE_COMMAND} -E chdir "${TAURI_APP_SOURCE_DIR}" ${NPM_COMMAND} run ${TAURI_BUILD_SCRIPT})
    else()
        set(TAURI_BUILD_COMMAND
            ${CMAKE_COMMAND} -E env "PATH=${TAURI_CARGO_BIN_DIR}:$ENV{PATH}"
            ${CMAKE_COMMAND} -E chdir "${TAURI_APP_SOURCE_DIR}" ${NPM_COMMAND} run ${TAURI_BUILD_SCRIPT})
    endif()

    # Note: On Windows, NPM_COMMAND uses 'cmd /c' wrapper to handle npm.cmd batch file
    # Note: Avoid shell-special characters in echo messages (no parentheses, etc.)
    add_custom_command(
        OUTPUT "${TAURI_APP_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TAURI_APP_UNPACKED_DIR}"
        COMMAND ${CMAKE_COMMAND} -E echo "============================================================"
        COMMAND ${CMAKE_COMMAND} -E echo "Building Tauri desktop app (cargo + webpack)"
        COMMAND ${CMAKE_COMMAND} -E echo "First build downloads ~80 Rust crates and compiles them with"
        COMMAND ${CMAKE_COMMAND} -E echo "LTO; expect several minutes the first time. Incremental"
        COMMAND ${CMAKE_COMMAND} -E echo "rebuilds are much faster. For UI iteration, prefer:"
        COMMAND ${CMAKE_COMMAND} -E echo "    cd src/app && npm run dev"
        COMMAND ${CMAKE_COMMAND} -E echo "============================================================"
        COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies for Tauri app..."
        COMMAND ${CMAKE_COMMAND} -E chdir "${TAURI_APP_SOURCE_DIR}" ${NPM_COMMAND} ci --ignore-scripts
        COMMAND ${CMAKE_COMMAND} -E echo "Compiling Rust host and bundling renderer..."
        COMMAND ${TAURI_BUILD_COMMAND}
        COMMAND ${CMAKE_COMMAND} -E echo "Staging Tauri build output into ${TAURI_APP_UNPACKED_DIR}..."
        COMMAND ${TAURI_COPY_COMMAND}
        DEPENDS ${TAURI_APP_SOURCES}
        COMMENT "Building Tauri app with cargo + webpack"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        VERBATIM
    )

    add_custom_target(tauri-app
        DEPENDS "${TAURI_APP_OUTPUT}"
    )

    message(STATUS "Node.js found: ${NODE_EXECUTABLE}")
    message(STATUS "npm found: ${NPM_EXECUTABLE}")
    message(STATUS "Cargo found: ${CARGO_EXECUTABLE}")
    if(WIN32)
        message(STATUS "Tauri app can be built with: cmake --build --preset windows --target tauri-app")
    else()
        message(STATUS "Tauri app can be built with: cmake --build --preset default --target tauri-app")
    endif()
else()
    if(NOT CARGO_EXECUTABLE)
        message(STATUS "Cargo not found - Tauri app build target disabled")
        message(STATUS "Install Rust via https://rustup.rs to enable")
    endif()
    if(NOT NODE_EXECUTABLE OR NOT NPM_EXECUTABLE)
        message(STATUS "Node.js or npm not found - Tauri app build target disabled")
        message(STATUS "Install Node.js to enable: https://nodejs.org/")
    endif()
endif()

# ============================================================
# Web App Integration (Cross-Platform)
# ============================================================

# Enable automatic web app build on all platforms (can be disabled with -DBUILD_WEB_APP=OFF)
option(BUILD_WEB_APP "Build the Web app automatically" ON)

# Check if webpack is available for system-package mode
if(USE_SYSTEM_NODEJS_MODULES)
    find_program(WEBPACK_EXECUTABLE webpack)
    if(WEBPACK_EXECUTABLE)
        message(STATUS "Using system webpack: ${WEBPACK_EXECUTABLE}")
        set(CAN_BUILD_WEB_APP TRUE)
    endif()
else()
    # Non-system mode requires npm for dependency install + build
    if(NODE_EXECUTABLE AND NPM_EXECUTABLE)
        set(CAN_BUILD_WEB_APP TRUE)
    endif()
endif()

# Custom target to build the Web app with proper dependency tracking
if(CAN_BUILD_WEB_APP AND BUILD_WEB_APP)
    # Create a stamp file to track build state
    set(WEB_APP_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-app.stamp")
    set(WEB_APP_BUILD_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/web-app-staging")

    # Track sources from BOTH src/app (the shared renderer) and src/web-app
    # (this build's package.json + webpack config) so the staged copies
    # rebuild whenever either tree changes.
    file(GLOB_RECURSE WEB_APP_SOURCES
        CONFIGURE_DEPENDS
        "${APP_SOURCE_DIR}/src/*"
        "${APP_SOURCE_DIR}/assets/*"
        "${APP_SOURCE_DIR}/styles/*"
        "${WEB_APP_SOURCE_DIR}/*.json"
        "${WEB_APP_SOURCE_DIR}/*.js"
        "${WEB_APP_SOURCE_DIR}/*.ts"
        "${WEB_APP_SOURCE_DIR}/*.tsx"
        "${WEB_APP_SOURCE_DIR}/*.css"
    )

    # Add custom command to build web app (only runs if sources change)
    add_custom_command(
        OUTPUT ${WEB_APP_STAMP}
        COMMAND ${CMAKE_COMMAND}
            -DAPP_SOURCE_DIR=${APP_SOURCE_DIR}
            -DWEB_APP_SOURCE_DIR=${WEB_APP_SOURCE_DIR}
            -DWEB_APP_BUILD_STAGING_DIR=${WEB_APP_BUILD_STAGING_DIR}
            -DWEB_APP_BUILD_DIR=${WEB_APP_BUILD_DIR}
            -DNPM_EXECUTABLE=${NPM_EXECUTABLE}
            -DWEBPACK_EXECUTABLE=${WEBPACK_EXECUTABLE}
            -DUSE_SYSTEM_NODEJS_MODULES=${USE_SYSTEM_NODEJS_MODULES}
            -DWEB_APP_STAMP=${WEB_APP_STAMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/src/web-app/BuildWebApp.cmake
        DEPENDS ${WEB_APP_SOURCES}
        COMMENT "Building Web app"
    )

    # Add custom target that depends on the stamp file
    add_custom_target(web-app ALL DEPENDS ${WEB_APP_STAMP})

    message(STATUS "Web app will be built automatically (disable with -DBUILD_WEB_APP=OFF)")

    if (NOT WIN32 AND NOT APPLE AND NOT BUILD_TAURI_APP)
        # Install .desktop file for web app
        install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade-web-app.desktop
            DESTINATION share/applications
        )
        # Install MetaInfo file for web app
        install(FILES ${CMAKE_SOURCE_DIR}/data/ai.lemonadeserver.webapp.metainfo.xml
            DESTINATION share/metainfo
        )
        # Create symlink in standard applications path only if not installing to /usr
        if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
            install(CODE "
                file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/applications\")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        ${CMAKE_INSTALL_PREFIX}/share/applications/lemonade-web-app.desktop
                        \"\$ENV{DESTDIR}/usr/share/applications/lemonade-web-app.desktop\"
                )
            ")
        endif()
        install(FILES ${CMAKE_SOURCE_DIR}/src/app/assets/logo.svg
            DESTINATION share/pixmaps
            RENAME lemonade-app.svg
        )
        # Create symlink in standard pixmaps path only if not installing to /usr
        if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
            install(CODE "
                file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/pixmaps\")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        ${CMAKE_INSTALL_PREFIX}/share/pixmaps/lemonade-app.svg
                        \"\$ENV{DESTDIR}/usr/share/pixmaps/lemonade-app.svg\"
                )
            ")
        endif()
        # Install launch script for web app
        install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade-web-app
            DESTINATION bin
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        )
        # Create symlink in standard bin path only if not installing to /usr
        if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
            install(CODE "
                file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        ${CMAKE_INSTALL_PREFIX}/bin/lemonade-web-app
                        \"\$ENV{DESTDIR}/usr/bin/lemonade-web-app\"
                )
            ")
        endif()
    endif()
else()
    if(NOT CAN_BUILD_WEB_APP)
        if(USE_SYSTEM_NODEJS_MODULES)
            message(STATUS "Web app build disabled - webpack not found (install webpack package)")
        else()
            message(STATUS "Web app build disabled - Node.js or npm not found")
        endif()
    else()
        message(STATUS "Web app automatic build disabled (enable with -DBUILD_WEB_APP=ON)")
    endif()
endif()

# Add manual web-app target for platforms where automatic build is disabled.
# Reuse the shared build script so WiX/MSBuild gets output under ${WEB_APP_BUILD_DIR}.
if(CAN_BUILD_WEB_APP AND NOT BUILD_WEB_APP)
    set(WEB_APP_BUILD_STAGING_DIR_MANUAL "${CMAKE_CURRENT_BINARY_DIR}/web-app-manual-staging")
    set(WEB_APP_MANUAL_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-app-manual.stamp")

    add_custom_target(web-app
        COMMAND ${CMAKE_COMMAND}
            -DAPP_SOURCE_DIR=${APP_SOURCE_DIR}
            -DWEB_APP_SOURCE_DIR=${WEB_APP_SOURCE_DIR}
            -DWEB_APP_BUILD_STAGING_DIR=${WEB_APP_BUILD_STAGING_DIR_MANUAL}
            -DWEB_APP_BUILD_DIR=${WEB_APP_BUILD_DIR}
            -DNPM_EXECUTABLE=${NPM_EXECUTABLE}
            -DWEBPACK_EXECUTABLE=${WEBPACK_EXECUTABLE}
            -DUSE_SYSTEM_NODEJS_MODULES=${USE_SYSTEM_NODEJS_MODULES}
            -DWEB_APP_STAMP=${WEB_APP_MANUAL_STAMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/src/web-app/BuildWebApp.cmake
        COMMENT "Building Web app"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        VERBATIM
    )

    message(STATUS "Web app can be built manually with: cmake --build . --target web-app")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Packaging.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embeddable.cmake)

# RecipeOptions precedence: verify that values are merged in the correct order:
#   1. Per-request settings (highest)
#   2. User-saved recipe options
#   3. Model recipe_options (registry entry)
#   4. Model image_defaults
#   5. Architecture defaults
#   6. Global config defaults (lowest)
set(_RECIPE_OPTIONS_PRECEDENCE_TEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/test_recipe_options_precedence.cpp")
if(BUILD_TESTING AND EXISTS "${_RECIPE_OPTIONS_PRECEDENCE_TEST_SRC}")
    add_executable(test_recipe_options_precedence test/cpp/test_recipe_options_precedence.cpp)
    target_link_libraries(test_recipe_options_precedence PRIVATE lemonade-server-core)
    add_cpp_ci_test(RecipeOptionsPrecedence CI ON COMMAND test_recipe_options_precedence)
endif()

set(_GLIBC_MALLOPT_PROBE_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/glibc_mallopt_probe.cpp"
)
set(_GLIBC_ALLOCATOR_POLICY_TEST_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/test/test_glibc_allocator_policy.py"
)
if(BUILD_TESTING AND
   CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   EXISTS "${_GLIBC_MALLOPT_PROBE_SRC}" AND
   EXISTS "${_GLIBC_ALLOCATOR_POLICY_TEST_SRC}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_library(test_glibc_mallopt_probe SHARED
            ${_GLIBC_MALLOPT_PROBE_SRC}
        )
        add_cpp_ci_test(
            GlibcAllocatorPolicyTest
            CI ON
            COMMAND
                ${Python3_EXECUTABLE}
                ${_GLIBC_ALLOCATOR_POLICY_TEST_SRC}
                --lemond $<TARGET_FILE:${EXECUTABLE_NAME}>
                --probe $<TARGET_FILE:test_glibc_mallopt_probe>
            DEPENDS
                ${EXECUTABLE_NAME}
                test_glibc_mallopt_probe
        )
        set_tests_properties(
            GlibcAllocatorPolicyTest
            PROPERTIES SKIP_RETURN_CODE 77
        )
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

set(_MODEL_SYNC_TEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/test_model_sync_and_auto_update.cpp")
if(BUILD_TESTING AND EXISTS "${_MODEL_SYNC_TEST_SRC}")
    add_executable(test_model_sync_and_auto_update test/cpp/test_model_sync_and_auto_update.cpp)
    target_link_libraries(test_model_sync_and_auto_update PRIVATE lemonade-server-core)
    add_dependencies(test_model_sync_and_auto_update copy_resources)

    include(CTest)
    add_cpp_ci_test(ModelSyncAndAutoUpdateTest CI ON COMMAND test_model_sync_and_auto_update)
endif()
