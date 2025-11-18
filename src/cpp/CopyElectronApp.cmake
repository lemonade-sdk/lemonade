# Cross-platform script to copy Electron app files
# This script is executed via cmake -P by the post-build step

# Check if the Electron app directory exists
if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}")
    message(STATUS "Found Electron app! Copying files to ${TARGET_DIR}...")
    
    # Copy the main Electron executable
    file(GLOB ELECTRON_MAIN_EXE "${ELECTRON_APP_UNPACKED_DIR}/${ELECTRON_EXE_NAME}")
    if(ELECTRON_MAIN_EXE)
        file(COPY ${ELECTRON_MAIN_EXE} DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied ${ELECTRON_EXE_NAME}")
    endif()
    
    # Copy DLL files (Windows)
    file(GLOB DLL_FILES "${ELECTRON_APP_UNPACKED_DIR}/*.dll")
    if(DLL_FILES)
        file(COPY ${DLL_FILES} DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied DLL files")
    endif()
    
    # Copy .so files (Linux)
    file(GLOB SO_FILES "${ELECTRON_APP_UNPACKED_DIR}/*.so*")
    if(SO_FILES)
        file(COPY ${SO_FILES} DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied shared library files")
    endif()
    
    # Copy .dylib files (macOS)
    file(GLOB DYLIB_FILES "${ELECTRON_APP_UNPACKED_DIR}/*.dylib")
    if(DYLIB_FILES)
        file(COPY ${DYLIB_FILES} DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied dynamic library files")
    endif()
    
    # Copy resource files (.pak, .bin, .dat, .json)
    file(GLOB RESOURCE_FILES 
        "${ELECTRON_APP_UNPACKED_DIR}/*.pak"
        "${ELECTRON_APP_UNPACKED_DIR}/*.bin"
        "${ELECTRON_APP_UNPACKED_DIR}/*.dat"
        "${ELECTRON_APP_UNPACKED_DIR}/*.json"
    )
    if(RESOURCE_FILES)
        file(COPY ${RESOURCE_FILES} DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied resource files")
    endif()
    
    # Copy locales directory
    if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/locales")
        file(COPY "${ELECTRON_APP_UNPACKED_DIR}/locales" 
             DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied locales directory")
    endif()
    
    # Copy resources directory (rename to electron-resources to avoid conflict)
    if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/resources")
        # Remove existing electron-resources if it exists
        if(EXISTS "${TARGET_DIR}/electron-resources")
            file(REMOVE_RECURSE "${TARGET_DIR}/electron-resources")
        endif()
        # Copy to electron-resources directly
        file(COPY "${ELECTRON_APP_UNPACKED_DIR}/resources/" 
             DESTINATION "${TARGET_DIR}/electron-resources")
        message(STATUS "  ✓ Copied Electron resources directory")
    endif()
    
    # Copy frameworks directory (macOS)
    if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/Frameworks")
        file(COPY "${ELECTRON_APP_UNPACKED_DIR}/Frameworks" 
             DESTINATION "${TARGET_DIR}")
        message(STATUS "  ✓ Copied Frameworks directory")
    endif()
    
    message(STATUS "Electron app copied successfully!")
else()
    message(STATUS "Electron app not found (this is optional).")
    message(STATUS "To build the Electron app, run:")
    message(STATUS "  cmake --build . --target electron-app")
    message(STATUS "Or manually: cd src/app && npm run build")
endif()

