# Tray application subproject
# Note: This is included from parent CMakeLists.txt via add_subdirectory(tray)
# Parent project already sets C++ standard and fetches dependencies

# Generate manifest files from templates (Windows only)
if(WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/lemonade-server.manifest.in
        ${CMAKE_CURRENT_BINARY_DIR}/lemonade-server.manifest
        @ONLY
    )
endif()

# Platform-specific sources
set(TRAY_SOURCES "")
set(TRAY_LIBS "")

if(WIN32)
    set(TRAY_SOURCES
        platform/windows_tray.cpp
    )
    set(TRAY_LIBS
        user32
        shell32
        ole32
        oleaut32
        comctl32
    )

    # Note: Not using WIN32 flag to support both --no-tray (console) and tray modes
    # For true GUI-only mode, could add WIN32 flag and use WinMain

elseif(APPLE)
    set(TRAY_SOURCES
        platform/macos_tray.mm
    )

    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(USERNOTIFICATIONS_LIBRARY UserNotifications REQUIRED)
    find_library(METAL_LIBRARY Metal REQUIRED)

    set(TRAY_LIBS
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${USERNOTIFICATIONS_LIBRARY}
        ${METAL_LIBRARY}
    )

    # Enable Objective-C++ compilation
    set_source_files_properties(
        platform/macos_tray.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )

elseif(UNIX)  # Linux
    # Check for Linux tray dependencies (GTK3, AppIndicator3)
    find_package(PkgConfig REQUIRED)

    # Allow explicitly enabling/disabling tray support from command line.
    # Default is OFF (auto-detect): tray is enabled if dependencies are found.
    # Set -DREQUIRE_LINUX_TRAY=ON to require tray support (fail if deps missing).
    option(REQUIRE_LINUX_TRAY "Enable Linux system tray support (requires AppIndicator3; GTK3 not needed when using the glib variant)" OFF)

    pkg_check_modules(GTK3 gtk+-3.0)

    # Prefer the newer GLib variant (GTK-free, recommended successor).
    # The module is named ayatana-appindicator-glib-0.1 upstream but some distros
    # (e.g. some Ubuntu releases) ship it without the version suffix.
    # Fall back to the Ayatana GTK3 variant, then upstream libappindicator3.
    pkg_check_modules(APP_INDICATOR_GLIB ayatana-appindicator-glib-0.1)
    if(NOT APP_INDICATOR_GLIB_FOUND)
        pkg_check_modules(APP_INDICATOR_GLIB ayatana-appindicator-glib)
    endif()
    if(NOT APP_INDICATOR_GLIB_FOUND)
        # Prefer Ayatana GTK3 variant, fall back to upstream libappindicator3.
        pkg_check_modules(APP_INDICATOR3 ayatana-appindicator3-0.1)
        if(APP_INDICATOR3_FOUND)
            set(HAVE_AYATANA_APPINDICATOR TRUE)
        else()
            pkg_check_modules(APP_INDICATOR3 appindicator3-0.1)
        endif()
    endif()

    pkg_check_modules(LIBNOTIFY libnotify)

    if(REQUIRE_LINUX_TRAY AND NOT (APP_INDICATOR_GLIB_FOUND OR (GTK3_FOUND AND APP_INDICATOR3_FOUND)))
        message(FATAL_ERROR "REQUIRE_LINUX_TRAY=ON but required dependencies not found. "
            "Install one of: ayatana-appindicator-glib-devel (GTK-free), "
            "or gtk3-devel + ayatana-appindicator3-devel / libappindicator-gtk3-devel.")
    endif()

    if(APP_INDICATOR_GLIB_FOUND OR (GTK3_FOUND AND APP_INDICATOR3_FOUND))
        set(_LINUX_TRAY_BUILD ON)

        if(APP_INDICATOR_GLIB_FOUND)
            # GTK-free path: only GIO/GLib required.
            set(TRAY_LIBS ${APP_INDICATOR_GLIB_LIBRARIES})
            set(TRAY_INCLUDE_DIRS ${APP_INDICATOR_GLIB_INCLUDE_DIRS})
            set(TRAY_DEFINITIONS HAVE_APPINDICATOR HAVE_AYATANA_APPINDICATOR_GLIB)
            # ayatana-appindicator-glib exports menus via org.gtk.Menus (GMenuModel),
            # but GNOME Shell's AppIndicator extension currently only speaks
            # com.canonical.dbusmenu.  We bridge the gap by also registering a
            # DbusmenuServer at the same D-Bus path.  Both interfaces coexist safely:
            # once GNOME Shell adopts org.gtk.Menus it will use that natively and
            # ignore the bridge.  dbusmenu-glib is optional — if absent, the bridge
            # is omitted and tray menus will not work on GNOME Shell.
            pkg_check_modules(DBUSMENU_GLIB dbusmenu-glib-0.4)
            if(DBUSMENU_GLIB_FOUND)
                list(APPEND TRAY_LIBS ${DBUSMENU_GLIB_LIBRARIES})
                list(APPEND TRAY_INCLUDE_DIRS ${DBUSMENU_GLIB_INCLUDE_DIRS})
                list(APPEND TRAY_DEFINITIONS HAVE_DBUSMENU_GLIB)
            else()
                message(WARNING "dbusmenu-glib-0.4 not found — tray menus will not "
                    "work on GNOME Shell (install libdbusmenu-glib-dev or equivalent)")
            endif()
        else()
            # GTK3 path.
            set(TRAY_LIBS
                ${GTK3_LIBRARIES}
                ${APP_INDICATOR3_LIBRARIES}
            )
            set(TRAY_INCLUDE_DIRS
                ${GTK3_INCLUDE_DIRS}
                ${APP_INDICATOR3_INCLUDE_DIRS}
            )
            set(TRAY_DEFINITIONS HAVE_APPINDICATOR)
            if(HAVE_AYATANA_APPINDICATOR)
                list(APPEND TRAY_DEFINITIONS HAVE_AYATANA_APPINDICATOR)
            endif()
        endif()

        if(LIBNOTIFY_FOUND)
            list(APPEND TRAY_LIBS ${LIBNOTIFY_LIBRARIES})
            list(APPEND TRAY_DEFINITIONS HAVE_LIBNOTIFY)
            list(APPEND TRAY_INCLUDE_DIRS ${LIBNOTIFY_INCLUDE_DIRS})
        endif()

    else()
        set(_LINUX_TRAY_BUILD OFF)
    endif()
endif()

# Common sources
set(COMMON_SOURCES
    main.cpp
    tray_app.cpp
    agent_launcher.cpp
    server_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/cli_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/recipe_options.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/system_info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/json_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/process_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/backends/backend_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/path_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/version_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/wmi_helper.cpp
    platform/tray_factory.cpp
)

# macOS-specific sources
if(APPLE)
    list(APPEND COMMON_SOURCES
        LemonadeServiceManager.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/macos_system_info.mm
    )

    # Enable Objective-C++ compilation for macOS files
    set_source_files_properties(
        LemonadeServiceManager.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/macos_system_info.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )
endif()

# Add version resource file on Windows
if(WIN32)
    list(APPEND COMMON_SOURCES version.rc)
endif()

# ============================================================
# lemonade-server - Console CLI / daemon
# ============================================================
add_executable(lemonade-server ${COMMON_SOURCES})

if(UNIX AND NOT APPLE)
    # Linux: always headless, no GTK deps — daemon-friendly
    target_sources(lemonade-server PRIVATE platform/linux_tray.cpp)
    # No TRAY_LIBS, no TRAY_DEFINITIONS, no TRAY_INCLUDE_DIRS
else()
    # Windows / macOS: use platform tray sources as before
    target_sources(lemonade-server PRIVATE ${TRAY_SOURCES})
    target_link_libraries(lemonade-server PRIVATE ${TRAY_LIBS})
    target_include_directories(lemonade-server PRIVATE ${TRAY_INCLUDE_DIRS})
    target_compile_definitions(lemonade-server PRIVATE ${TRAY_DEFINITIONS})
endif()

target_link_libraries(lemonade-server PRIVATE
    nlohmann_json::nlohmann_json
)

target_include_directories(lemonade-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

if(USE_SYSTEM_HTTPLIB)
    target_link_libraries(lemonade-server PRIVATE cpp-httplib)
else()
    target_link_libraries(lemonade-server PRIVATE httplib::httplib)
endif()

# Link CLI11 based on what's available
if(USE_SYSTEM_CLI11)
    target_include_directories(lemonade-server PRIVATE ${CLI11_INCLUDE_DIRS})
else()
    target_link_libraries(lemonade-server PRIVATE CLI11::CLI11)
endif()

if(USE_SYSTEM_HTTPLIB AND HTTPLIB_INCLUDE_DIRS)
    target_include_directories(lemonade-server PRIVATE ${HTTPLIB_INCLUDE_DIRS})
endif()

target_compile_definitions(lemonade-server PRIVATE
    LEMONADE_TRAY
    $<$<PLATFORM_ID:Linux>:LEMONADE_NO_TRAY_SUBCOMMAND>
)

# Enable ARC (Automatic Reference Counting) for macOS Objective-C++ files
if(APPLE)
    target_compile_options(lemonade-server PRIVATE -fobjc-arc)
endif()

# Set output directory to build root for all platforms
# Windows uses Debug/Release subdirectories (multi-config generator)
# Linux/Mac go directly to CMAKE_BINARY_DIR (single-config generator)
set_target_properties(lemonade-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    set_target_properties(lemonade-server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

# ============================================================
# lemonade-tray.exe - GUI tray launcher (Windows only)
# ============================================================
if(WIN32)
    add_executable(lemonade-tray WIN32
        tray_launcher.cpp
    )

    # Add version resource for tray launcher
    target_sources(lemonade-tray PRIVATE version.rc)

    # Include directories for single_instance.h
    target_include_directories(lemonade-tray PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    target_link_libraries(lemonade-tray PRIVATE
        nlohmann_json::nlohmann_json
        user32
        shell32
    )

    set_target_properties(lemonade-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )

    # ============================================================
    # lemonade-log-viewer.exe - Log file viewer (Windows only)
    # ============================================================
    add_executable(lemonade-log-viewer
        log-viewer.cpp
    )

    target_link_libraries(lemonade-log-viewer PRIVATE
        nlohmann_json::nlohmann_json
    )

    set_target_properties(lemonade-log-viewer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

# ============================================================
# lemonade-tray - Full GUI tray application (Linux only)
# ============================================================
if(UNIX AND NOT APPLE AND _LINUX_TRAY_BUILD)
    add_executable(lemonade-tray
        ${COMMON_SOURCES}
        platform/linux_tray.cpp
    )

    target_link_libraries(lemonade-tray PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
        ${TRAY_LIBS}
    )
    if(USE_SYSTEM_HTTPLIB)
        target_link_libraries(lemonade-tray PRIVATE cpp-httplib)
    else()
        target_link_libraries(lemonade-tray PRIVATE httplib::httplib)
    endif()

    target_include_directories(lemonade-tray PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${TRAY_INCLUDE_DIRS}
    )

    target_compile_definitions(lemonade-tray PRIVATE
        LEMONADE_TRAY
        LEMONADE_TRAY_GUI
        ${TRAY_DEFINITIONS}
    )

    # CLI11 linkage (mirror lemonade-server)
    if(USE_SYSTEM_CLI11)
        target_include_directories(lemonade-tray PRIVATE ${CLI11_INCLUDE_DIRS})
    else()
        target_link_libraries(lemonade-tray PRIVATE CLI11::CLI11)
    endif()

    # httplib linkage (mirror lemonade-server)
    if(USE_SYSTEM_HTTPLIB AND HTTPLIB_INCLUDE_DIRS)
        target_include_directories(lemonade-tray PRIVATE ${HTTPLIB_INCLUDE_DIRS})
    endif()

    # systemd support (mirror lemonade-server's existing block)
    if(SYSTEMD_FOUND)
        target_include_directories(lemonade-tray PRIVATE ${SYSTEMD_INCLUDE_DIRS})
        target_link_libraries(lemonade-tray PRIVATE ${SYSTEMD_LIBRARIES})
        target_compile_definitions(lemonade-tray PRIVATE
            HAVE_SYSTEMD
            LEMONADE_SYSTEMD_UNIT_NAME="${LEMONADE_SYSTEMD_UNIT_NAME}")
    endif()

    set_target_properties(lemonade-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    install(TARGETS lemonade-tray RUNTIME DESTINATION bin)

    # Symlink in /usr/bin if not installing to /usr
    # Use a relative symlink target so the .deb works correctly regardless of DESTDIR.
    if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        install(CODE "
            file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
            file(RELATIVE_PATH _symlink_target \"/usr/bin\" \"${CMAKE_INSTALL_PREFIX}/bin/lemonade-tray\")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    \"\${_symlink_target}\"
                    \"\$ENV{DESTDIR}/usr/bin/lemonade-tray\"
            )
        ")
    endif()
endif()

# Platform-specific settings
if(WIN32)
    # On Windows, make sure we have Unicode support
    target_compile_definitions(lemonade-server PRIVATE
        UNICODE
        _UNICODE
    )

    # Link with networking and system libraries
    target_link_libraries(lemonade-server PRIVATE
        ws2_32     # Winsock
        iphlpapi   # IP Helper API for network connection enumeration
    )

    # Embed manifest file
    if(MSVC)
        set_target_properties(lemonade-server PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/lemonade-server.manifest"
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    # On Linux, link with pthread
    find_package(Threads REQUIRED)
    target_link_libraries(lemonade-server PRIVATE
        Threads::Threads
    )

    # Optional: systemd detection for service status
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SYSTEMD QUIET libsystemd)
        if(SYSTEMD_FOUND)
            if(NOT DEFINED LEMONADE_SYSTEMD_UNIT_NAME)
                set(LEMONADE_SYSTEMD_UNIT_NAME "lemonade-server.service" CACHE STRING
                    "Systemd unit name to match for journald log mode (overridable at runtime via LEMONADE_SYSTEMD_UNIT env var)")
            endif()
            target_include_directories(lemonade-server PRIVATE ${SYSTEMD_INCLUDE_DIRS})
            target_link_libraries(lemonade-server PRIVATE ${SYSTEMD_LIBRARIES})
            target_compile_definitions(lemonade-server PRIVATE
                HAVE_SYSTEMD
                LEMONADE_SYSTEMD_UNIT_NAME="${LEMONADE_SYSTEMD_UNIT_NAME}")
        endif()
    endif()
endif()

# Install target (only on non-macOS platforms where main CMakeLists doesn't handle it)
if(NOT APPLE)
    install(TARGETS lemonade-server
        RUNTIME DESTINATION bin
    )
endif()

# ============================================================
# macOS Signing (Must be in this file where target is defined)
# ============================================================
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Configuring signing for Tray Application (lemonade-server)")

    # Inherit variables from Parent CMake
    if(NOT DEFINED SIGNING_IDENTITY OR NOT DEFINED ENTITLEMENTS_PATH)
        message(FATAL_ERROR "Signing variables missing! Ensure add_subdirectory(tray) is called AFTER identities are set in root CMakeLists.txt")
    endif()

    add_custom_command(TARGET lemonade-server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "--- Signing lemonade-server (Tray) ---"
        COMMAND codesign --force --options runtime --timestamp --entitlements "${ENTITLEMENTS_PATH}" --sign "${SIGNING_IDENTITY}" -v $<TARGET_FILE:lemonade-server>
        COMMENT "Signing lemonade-server with Hardened Runtime"
        VERBATIM
    )
endif()

# Create symlink in standard bin path only if not installing to /usr
if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_INSTALL_PREFIX}/bin/lemonade-server
                \"\$ENV{DESTDIR}/usr/bin/lemonade-server\"
        )
    ")
endif()

# Note: Icon resources are copied by parent CMakeLists.txt

# Print configuration summary
message(STATUS "=== Lemonade Tray Application Configuration ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(WIN32)
    message(STATUS "Windows tray support: YES")
elseif(APPLE)
    message(STATUS "macOS tray support: PARTIAL (stub)")
elseif(UNIX)
    message(STATUS "lemonade-server: GTK-free (headless, daemon-friendly)")
    if(_LINUX_TRAY_BUILD)
        if(APP_INDICATOR_GLIB_FOUND)
            message(STATUS "lemonade-tray: ENABLED (ayatana-appindicator-glib, GTK-free)")
        else()
            message(STATUS "lemonade-tray: ENABLED (GTK3 + AppIndicator3)")
        endif()
        if(LIBNOTIFY_FOUND)
            message(STATUS "  Notifications: ENABLED (libnotify)")
        else()
            message(STATUS "  Notifications: DISABLED")
        endif()
    else()
        message(STATUS "lemonade-tray: NOT BUILT (AppIndicator3 not found)")
    endif()
endif()
message(STATUS "===============================================")
