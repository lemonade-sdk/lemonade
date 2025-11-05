# Tray application subproject
# Note: This is included from parent CMakeLists.txt via add_subdirectory(tray)
# Parent project already sets C++ standard and fetches dependencies

# Generate manifest files from templates (Windows only)
if(WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/lemonade-server-beta.manifest.in
        ${CMAKE_CURRENT_BINARY_DIR}/lemonade-server-beta.manifest
        @ONLY
    )
endif()

# Platform-specific sources
set(PLATFORM_SOURCES "")
set(PLATFORM_LIBS "")

if(WIN32)
    set(PLATFORM_SOURCES
        platform/windows_tray.cpp
    )
    set(PLATFORM_LIBS
        user32 
        shell32 
        ole32 
        comctl32
    )
    
    # Note: Not using WIN32 flag to support both --no-tray (console) and tray modes
    # For true GUI-only mode, could add WIN32 flag and use WinMain
    
elseif(APPLE)
    set(PLATFORM_SOURCES
        platform/macos_tray.mm
    )
    
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    
    set(PLATFORM_LIBS
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
    )
    
    # Enable Objective-C++ compilation
    set_source_files_properties(
        platform/macos_tray.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )
    
elseif(UNIX)  # Linux
    # Linux builds are headless-only (no tray support)
    # This avoids LGPL dependencies (GTK3, libappindicator3, libnotify)
    message(STATUS "Linux build: Tray application disabled (headless mode only)")
    message(STATUS "Use 'lemonade-server-beta serve --no-tray' to run server")
    
    # Include stub Linux tray implementation (headless mode)
    set(PLATFORM_SOURCES
        platform/linux_tray.cpp
    )
    set(PLATFORM_LIBS "")
endif()

# Common sources
set(COMMON_SOURCES
    main.cpp
    tray_app.cpp
    server_manager.cpp
    platform/tray_factory.cpp
)

# Add version resource file on Windows
if(WIN32)
    list(APPEND COMMON_SOURCES version.rc)
endif()

# ============================================================
# lemonade-server-beta.exe - Console CLI client
# ============================================================
add_executable(lemonade-server-beta
    ${COMMON_SOURCES}
    ${PLATFORM_SOURCES}
)

target_include_directories(lemonade-server-beta PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(lemonade-server-beta PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    ${PLATFORM_LIBS}
)

# Set output directory to build root for all platforms
# Windows uses Debug/Release subdirectories (multi-config generator)
# Linux/Mac go directly to CMAKE_BINARY_DIR (single-config generator)
set_target_properties(lemonade-server-beta PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    set_target_properties(lemonade-server-beta PROPERTIES
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
    
    set_target_properties(lemonade-log-viewer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

# Platform-specific settings
if(WIN32)
    # On Windows, make sure we have Unicode support
    target_compile_definitions(lemonade-server-beta PRIVATE
        UNICODE
        _UNICODE
    )
    
    # Link with networking and system libraries
    target_link_libraries(lemonade-server-beta PRIVATE
        ws2_32     # Winsock
        iphlpapi   # IP Helper API for network connection enumeration
    )
    
    # Embed manifest file
    if(MSVC)
        set_target_properties(lemonade-server-beta PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/lemonade-server-beta.manifest"
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    # On Linux, link with pthread
    find_package(Threads REQUIRED)
    target_link_libraries(lemonade-server-beta PRIVATE
        Threads::Threads
    )
endif()

# Install target
install(TARGETS lemonade-server-beta
    RUNTIME DESTINATION bin
)

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
    message(STATUS "Linux tray support: HEADLESS ONLY (use --no-tray)")
    message(STATUS "  No LGPL dependencies - permissively licensed only")
endif()
message(STATUS "===============================================")

