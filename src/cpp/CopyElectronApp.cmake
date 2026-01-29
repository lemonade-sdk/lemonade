# Cross-platform script to copy Electron app files
# This script is executed via cmake -P by the post-build step
# Note: The Electron app is now built directly in build/electron-dist/,
# but this script still copies it to the final executable directory for convenience

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

    # Copy Electron resources directory and merge with existing resources
    if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/resources")
        # Copy app.asar to resources directory (required by Electron)
        if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/resources/app.asar")
            file(COPY "${ELECTRON_APP_UNPACKED_DIR}/resources/app.asar"
                 DESTINATION "${TARGET_DIR}/resources")
            message(STATUS "  ✓ Copied app.asar to resources")
        endif()

        # Copy dist directory (backend server and renderer)
        if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/resources/dist")
            file(COPY "${ELECTRON_APP_UNPACKED_DIR}/resources/dist"
                 DESTINATION "${TARGET_DIR}/resources")
            message(STATUS "  ✓ Copied dist directory to resources")
        endif()

        # Copy elevate.exe if it exists (Windows privilege elevation utility)
        if(EXISTS "${ELECTRON_APP_UNPACKED_DIR}/resources/elevate.exe")
            file(COPY "${ELECTRON_APP_UNPACKED_DIR}/resources/elevate.exe"
                 DESTINATION "${TARGET_DIR}/resources")
            message(STATUS "  ✓ Copied elevate.exe to resources")
        endif()

        message(STATUS "  ✓ Merged Electron resources with server resources")
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
    message(STATUS "  cmake --build --preset default --target electron-app")
    message(STATUS "Or manually: cd src/app && npm run build")
endif()
