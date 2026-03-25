# Tray application subproject
# Note: This is included from parent CMakeLists.txt via add_subdirectory(tray)
# Parent project already sets C++ standard and fetches dependencies
#
# Targets:
#   Windows: LemonadeServer.exe (SUBSYSTEM:WINDOWS, embeds server via lemonade-server-core)
#   macOS:   lemonade-tray (lightweight tray client, connects to running router)
#   Linux:   lemonade-tray (lightweight tray client, when AppIndicator3 found)

# ============================================================
# Platform-specific tray sources and dependencies
# ============================================================

set(TRAY_PLATFORM_SOURCES "")
set(TRAY_PLATFORM_LIBS "")
set(TRAY_INCLUDE_DIRS "")
set(TRAY_DEFINITIONS "")

if(WIN32)
    set(TRAY_PLATFORM_SOURCES
        platform/windows_tray.cpp
    )
    set(TRAY_PLATFORM_LIBS
        user32
        shell32
        ole32
        oleaut32
        comctl32
    )

elseif(APPLE)
    set(TRAY_PLATFORM_SOURCES
        platform/macos_tray.mm
    )

    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(USERNOTIFICATIONS_LIBRARY UserNotifications REQUIRED)
    find_library(METAL_LIBRARY Metal REQUIRED)

    set(TRAY_PLATFORM_LIBS
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${USERNOTIFICATIONS_LIBRARY}
        ${METAL_LIBRARY}
    )

    set_source_files_properties(
        platform/macos_tray.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )

elseif(UNIX)  # Linux
    # Check for Linux tray dependencies (GTK3, AppIndicator3)
    find_package(PkgConfig REQUIRED)

    option(REQUIRE_LINUX_TRAY "Enable Linux system tray support (requires AppIndicator3; GTK3 not needed when using the glib variant)" OFF)

    pkg_check_modules(GTK3 gtk+-3.0)

    # Prefer the newer GLib variant (GTK-free, recommended successor).
    pkg_check_modules(APP_INDICATOR_GLIB ayatana-appindicator-glib-0.1)
    if(NOT APP_INDICATOR_GLIB_FOUND)
        pkg_check_modules(APP_INDICATOR_GLIB ayatana-appindicator-glib)
    endif()
    if(NOT APP_INDICATOR_GLIB_FOUND)
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
            set(TRAY_PLATFORM_LIBS ${APP_INDICATOR_GLIB_LIBRARIES})
            set(TRAY_INCLUDE_DIRS ${APP_INDICATOR_GLIB_INCLUDE_DIRS})
            set(TRAY_DEFINITIONS HAVE_APPINDICATOR HAVE_AYATANA_APPINDICATOR_GLIB)
            pkg_check_modules(DBUSMENU_GLIB dbusmenu-glib-0.4)
            if(DBUSMENU_GLIB_FOUND)
                list(APPEND TRAY_PLATFORM_LIBS ${DBUSMENU_GLIB_LIBRARIES})
                list(APPEND TRAY_INCLUDE_DIRS ${DBUSMENU_GLIB_INCLUDE_DIRS})
                list(APPEND TRAY_DEFINITIONS HAVE_DBUSMENU_GLIB)
            else()
                message(WARNING "dbusmenu-glib-0.4 not found — tray menus will not "
                    "work on GNOME Shell (install libdbusmenu-glib-dev or equivalent)")
            endif()
        else()
            set(TRAY_PLATFORM_LIBS
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
            list(APPEND TRAY_PLATFORM_LIBS ${LIBNOTIFY_LIBRARIES})
            list(APPEND TRAY_DEFINITIONS HAVE_LIBNOTIFY)
            list(APPEND TRAY_INCLUDE_DIRS ${LIBNOTIFY_INCLUDE_DIRS})
        endif()

        set(TRAY_PLATFORM_SOURCES platform/linux_tray.cpp)
    else()
        set(_LINUX_TRAY_BUILD OFF)
    endif()
endif()

# ============================================================
# Common tray UI sources (shared across all platform targets)
# ============================================================
set(TRAY_UI_SOURCES
    tray_ui.cpp
    platform/tray_factory.cpp
)

# path_utils and json_utils are needed by TrayUI but on Windows they're already in lemonade-server-core
if(NOT WIN32)
    list(APPEND TRAY_UI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/path_utils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/utils/json_utils.cpp
    )
endif()

# Helper function: apply lightweight tray deps (httplib, json, CLI11) to a target
function(apply_tray_deps target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )

    target_link_libraries(${target_name} PRIVATE nlohmann_json::nlohmann_json)

    if(USE_SYSTEM_HTTPLIB)
        target_link_libraries(${target_name} PRIVATE cpp-httplib)
    else()
        target_link_libraries(${target_name} PRIVATE httplib::httplib)
    endif()
    if(USE_SYSTEM_HTTPLIB AND HTTPLIB_INCLUDE_DIRS)
        target_include_directories(${target_name} PRIVATE ${HTTPLIB_INCLUDE_DIRS})
    endif()

    if(USE_SYSTEM_CLI11)
        target_include_directories(${target_name} PRIVATE ${CLI11_INCLUDE_DIRS})
    else()
        target_link_libraries(${target_name} PRIVATE CLI11::CLI11)
    endif()
endfunction()

# ============================================================
# Windows: LemonadeServer.exe (SUBSYSTEM:WINDOWS, embedded server)
# ============================================================
if(WIN32)
    add_executable(LemonadeServer WIN32
        main.cpp
        ${TRAY_UI_SOURCES}
        ${TRAY_PLATFORM_SOURCES}
    )

    # Add version resource
    target_sources(LemonadeServer PRIVATE version.rc)

    # Embedded server: link the full server core OBJECT library
    target_link_libraries(LemonadeServer PRIVATE lemonade-server-core)

    # Tray platform libraries
    target_link_libraries(LemonadeServer PRIVATE ${TRAY_PLATFORM_LIBS})

    # Lightweight tray deps (httplib, json, CLI11)
    apply_tray_deps(LemonadeServer)

    # Unicode support
    target_compile_definitions(LemonadeServer PRIVATE UNICODE _UNICODE)

    set_target_properties(LemonadeServer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )

    # Copy resources to LemonadeServer.exe output directory
    add_custom_command(TARGET LemonadeServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/resources
            $<TARGET_FILE_DIR:LemonadeServer>/resources
        COMMENT "Copying resources to LemonadeServer output directory"
    )
    add_dependencies(LemonadeServer copy_resources)
endif()

# ============================================================
# macOS: lemonade-tray (lightweight tray client)
# ============================================================
if(APPLE)
    add_executable(lemonade-tray
        main.cpp
        ${TRAY_UI_SOURCES}
        ${TRAY_PLATFORM_SOURCES}
    )

    apply_tray_deps(lemonade-tray)
    target_link_libraries(lemonade-tray PRIVATE ${TRAY_PLATFORM_LIBS})

    # Enable ARC for Objective-C++ files
    target_compile_options(lemonade-tray PRIVATE -fobjc-arc)

    set_target_properties(lemonade-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

# ============================================================
# Linux: lemonade-tray (lightweight tray client, when AppIndicator3 found)
# ============================================================
if(UNIX AND NOT APPLE AND _LINUX_TRAY_BUILD)
    add_executable(lemonade-tray
        main.cpp
        ${TRAY_UI_SOURCES}
        ${TRAY_PLATFORM_SOURCES}
    )

    apply_tray_deps(lemonade-tray)

    find_package(Threads REQUIRED)
    target_link_libraries(lemonade-tray PRIVATE
        Threads::Threads
        ${TRAY_PLATFORM_LIBS}
    )

    target_include_directories(lemonade-tray PRIVATE ${TRAY_INCLUDE_DIRS})
    target_compile_definitions(lemonade-tray PRIVATE ${TRAY_DEFINITIONS})

    set_target_properties(lemonade-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    install(TARGETS lemonade-tray RUNTIME DESTINATION bin)

    # No autostart desktop entry — Linux users start lemonade-tray manually

    # Symlink in /usr/bin if not installing to /usr
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

# ============================================================
# macOS Signing (Release builds only)
# ============================================================
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Configuring signing for Tray Application (lemonade-tray)")

    if(NOT DEFINED SIGNING_IDENTITY OR NOT DEFINED ENTITLEMENTS_PATH)
        message(FATAL_ERROR "Signing variables missing! Ensure add_subdirectory(tray) is called AFTER identities are set in root CMakeLists.txt")
    endif()

    add_custom_command(TARGET lemonade-tray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "--- Signing lemonade-tray ---"
        COMMAND codesign --force --options runtime --timestamp --entitlements "${ENTITLEMENTS_PATH}" --sign "${SIGNING_IDENTITY}" -v $<TARGET_FILE:lemonade-tray>
        COMMENT "Signing lemonade-tray with Hardened Runtime"
        VERBATIM
    )
endif()

# ============================================================
# Configuration summary
# ============================================================
message(STATUS "=== Lemonade Tray Configuration ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
if(WIN32)
    message(STATUS "LemonadeServer.exe (embedded server + tray): YES")
elseif(APPLE)
    message(STATUS "lemonade-tray (client): YES")
elseif(UNIX)
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
message(STATUS "====================================")
