# Tray application subproject
# Note: This is included from parent CMakeLists.txt via add_subdirectory(tray)
# Parent project already sets C++ standard and fetches dependencies

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
    find_package(PkgConfig REQUIRED)
    
    # Try to find GTK3 and libappindicator
    pkg_check_modules(GTK3 gtk+-3.0)
    pkg_check_modules(APPINDICATOR3 appindicator3-0.1)
    pkg_check_modules(NOTIFY libnotify)
    
    if(GTK3_FOUND AND APPINDICATOR3_FOUND AND NOTIFY_FOUND)
        set(PLATFORM_SOURCES
            platform/linux_tray.cpp
        )
        
        set(PLATFORM_LIBS
            ${GTK3_LIBRARIES}
            ${APPINDICATOR3_LIBRARIES}
            ${NOTIFY_LIBRARIES}
        )
        
        include_directories(
            ${GTK3_INCLUDE_DIRS}
            ${APPINDICATOR3_INCLUDE_DIRS}
            ${NOTIFY_INCLUDE_DIRS}
        )
        
        message(STATUS "Linux tray dependencies found")
    else()
        message(WARNING "Linux tray dependencies not found. Install with:")
        message(WARNING "  sudo apt-get install libappindicator3-dev libgtk-3-dev libnotify-dev")
        message(WARNING "The tray application will be built without Linux support.")
    endif()
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

# Create executable as Windows GUI app (no console window)
if(WIN32)
    add_executable(lemonade-server-beta WIN32
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
    )
else()
    add_executable(lemonade-server-beta
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
    )
endif()

# Include directories (use parent's unified include directory)
target_include_directories(lemonade-server-beta PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Link libraries
target_link_libraries(lemonade-server-beta PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    ${PLATFORM_LIBS}
)

# Set output directory to match parent build directory
# This ensures both lemonade-router.exe and lemonade-server-beta.exe are in the same folder
set_target_properties(lemonade-server-beta PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# Platform-specific settings
if(WIN32)
    # On Windows, make sure we have Unicode support
    target_compile_definitions(lemonade-server-beta PRIVATE
        UNICODE
        _UNICODE
    )
    
    # Link with threading library
    target_link_libraries(lemonade-server-beta PRIVATE
        ws2_32  # Winsock for future HTTP client
    )
    
    # Embed manifest file
    if(MSVC)
        set_target_properties(lemonade-server-beta PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/lemonade-server-beta.manifest"
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
    if(GTK3_FOUND AND APPINDICATOR3_FOUND)
        message(STATUS "Linux tray support: PARTIAL (stub)")
    else()
        message(STATUS "Linux tray support: NO (missing dependencies)")
    endif()
endif()
message(STATUS "===============================================")

