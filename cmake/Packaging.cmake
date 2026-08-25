# Keep the former root ordering while platform-specific logic stays in its owner file.
include(${CMAKE_CURRENT_LIST_DIR}/PackagingMacOS.cmake)

lemonade_prepare_macos_packaging()

# Add CLI application subdirectory
add_subdirectory(src/cpp/cli)

# Add tray application subdirectory
add_subdirectory(src/cpp/tray)

if(WIN32)
    lemonade_finalize_windows_packaging()
endif()

lemonade_finalize_macos_packaging()

include(${CMAKE_CURRENT_LIST_DIR}/PackagingLinux.cmake)
