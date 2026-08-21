# Export compile_commands.json for clangd/IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable Objective-C++ for macOS Metal support
if(APPLE)
    enable_language(OBJCXX)
endif()

# Prevent finding Homebrew libraries on macOS to use system ones
if(APPLE)
    set(CMAKE_IGNORE_PATH "/opt/homebrew/lib" "/opt/homebrew/include")
endif()
# Set default install prefix to /opt on Linux if not specified
if(UNIX AND NOT APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/opt" CACHE PATH "Install prefix" FORCE)
endif()

# Include GNUInstallDirs for standard installation directories
include(GNUInstallDirs)
     # Install man pages
     install(FILES
          ${CMAKE_SOURCE_DIR}/docs/man/man1/lemond.1
          ${CMAKE_SOURCE_DIR}/docs/man/man1/lemonade.1
          DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
     )

include(${CMAKE_CURRENT_LIST_DIR}/Backends.cmake)

# ============================================================
# Tauri app source paths (used by both runtime and installers)
# ============================================================
get_filename_component(TAURI_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/app" ABSOLUTE)
set(TAURI_APP_CARGO_DIR "${TAURI_APP_SOURCE_DIR}/src-tauri")
set(TAURI_APP_CARGO_TARGET "${TAURI_APP_CARGO_DIR}/target/release")
# Staging directory that mirrors the Electron layout the installers expect.
# A post-build copy step lands the Tauri output here so downstream logic
# (WiX fragment generation, CPack component install, etc.) doesn't need to
# know where Cargo puts its artifacts.
set(TAURI_APP_BUILD_DIR "${CMAKE_BINARY_DIR}/app")
if(WIN32)
    set(TAURI_APP_UNPACKED_DIR "${TAURI_APP_BUILD_DIR}")
    set(TAURI_EXE_NAME "lemonade-app.exe")
elseif(APPLE)
    set(TAURI_APP_UNPACKED_DIR "${TAURI_APP_BUILD_DIR}")
    set(TAURI_EXE_NAME "lemonade-app.app")
else()
    set(TAURI_APP_UNPACKED_DIR "${TAURI_APP_BUILD_DIR}")
    set(TAURI_EXE_NAME "lemonade-app")
endif()

# ============================================================
# Web App source paths
# ============================================================
get_filename_component(WEB_APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/web-app" ABSOLUTE)
get_filename_component(APP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/app" ABSOLUTE)
set(WEB_APP_BUILD_DIR "${CMAKE_BINARY_DIR}/resources/web-app")

# When ON, use system-provided Node.js modules (for example, webpack and distro-packaged JS deps)
# instead of installing project-local dependencies with npm for web-app builds.
option(USE_SYSTEM_NODEJS_MODULES "Use system-installed Node.js modules for building the Web app" OFF)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
