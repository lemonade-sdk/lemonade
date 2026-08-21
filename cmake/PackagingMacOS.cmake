# ============================================================
# macOS Packaging & Notarization
# ============================================================
if(APPLE)
    # SETUP IDENTITIES
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Lemonade")
    set(CPACK_PACKAGE_CONTACT "lemonade@amd.com")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lemonade Local LLM Server")
    set(CPACK_PACKAGE_DESCRIPTION "A lightweight, high-performance local LLM server.")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/../../LICENSE")
    # Skip readme page entirely
    set(CPACK_INSTALL_README_DIR "")

    if(NOT DEFINED SIGNING_IDENTITY)
        if(DEFINED ENV{DEVELOPER_ID_APPLICATION_IDENTITY})
            set(SIGNING_IDENTITY "$ENV{DEVELOPER_ID_APPLICATION_IDENTITY}")
        else()
            # Ad-hoc sign so local dev builds work without an Apple Developer cert.
            # Distribution builds set DEVELOPER_ID_APPLICATION_IDENTITY explicitly.
            set(SIGNING_IDENTITY "-")
        endif()
    endif()

    if(NOT DEFINED INSTALLER_IDENTITY)
        if(DEFINED ENV{DEVELOPER_ID_INSTALLER_IDENTITY})
            set(INSTALLER_IDENTITY "$ENV{DEVELOPER_ID_INSTALLER_IDENTITY}")
        else()
            set(INSTALLER_IDENTITY "B69167355F20D364EE8FD7126D767B93B5344EBB")
        endif()
    endif()

    # Configure Plists & Entitlements
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/ai.lemonadeserver.server.plist.in ${CMAKE_CURRENT_BINARY_DIR}/ai.lemonadeserver.server.plist @ONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/ai.lemonadeserver.tray.plist.in ${CMAKE_CURRENT_BINARY_DIR}/ai.lemonadeserver.tray.plist @ONLY)

    set(ENTITLEMENTS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/entitlements.plist")
    if(NOT EXISTS "${ENTITLEMENTS_PATH}")
        file(WRITE "${ENTITLEMENTS_PATH}"
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
            <!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
            <plist version=\"1.0\">
            <dict>
                <key>com.apple.security.cs.allow-jit</key><true/>
                <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
                <key>com.apple.security.cs.disable-library-validation</key><true/>
                <key>com.apple.security.cs.allow-dyld-environment-variables</key><true/>
            </dict>
            </plist>")
    endif()
endif()

# Add CLI application subdirectory
add_subdirectory(src/cpp/cli)

# Add tray application subdirectory
add_subdirectory(src/cpp/tray)

# Wire WiX installer targets to the binaries and frontend artifacts they package.
# Doing this after the subdirectories are added ensures MSBuild gets real project refs.
if(WIN32 AND WIX_EXECUTABLE)
    set(WIX_PACKAGED_TARGETS)

    if(TARGET LemonadeServer)
        list(APPEND WIX_PACKAGED_TARGETS LemonadeServer)
    endif()
    if(TARGET lemonade)
        list(APPEND WIX_PACKAGED_TARGETS lemonade)
    endif()
    if(TARGET ${EXECUTABLE_NAME})
        list(APPEND WIX_PACKAGED_TARGETS ${EXECUTABLE_NAME})
    endif()

    if(TARGET wix_installer_minimal)
        if(TARGET web-app)
            add_dependencies(wix_installer_minimal web-app)
        endif()
        if(WIX_PACKAGED_TARGETS)
            add_dependencies(wix_installer_minimal ${WIX_PACKAGED_TARGETS})
        endif()
    endif()

    if(TARGET wix_installer_full)
        if(TARGET tauri-app)
            add_dependencies(wix_installer_full tauri-app)
        endif()
        if(TARGET web-app)
            add_dependencies(wix_installer_full web-app)
        endif()
        if(WIX_PACKAGED_TARGETS)
            add_dependencies(wix_installer_full ${WIX_PACKAGED_TARGETS})
        endif()
    endif()
endif()

if(APPLE)
    # SIGNING LOGIC (Release Only)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "Release Build: Configuring Hardened Runtime Signing for Router")

        # Sign 'lemond'
        add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "--- Signing ${EXECUTABLE_NAME} ---"
            COMMAND codesign --force --options runtime --timestamp --entitlements "${ENTITLEMENTS_PATH}" --sign "${SIGNING_IDENTITY}" -v $<TARGET_FILE:${EXECUTABLE_NAME}>
            COMMENT "Signing ${EXECUTABLE_NAME} with Hardened Runtime"
            VERBATIM
        )
    endif()

    # Prepare Tauri App
    set(TAURI_APP_BUILD_COPY "${CMAKE_BINARY_DIR}/lemonade-app.app")
    add_custom_target(prepare_tauri_app
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${TAURI_APP_BUILD_COPY}"
        COMMAND cp -R "${TAURI_APP_UNPACKED_DIR}/lemonade-app.app" "${CMAKE_BINARY_DIR}/" || echo "Warning: Could not find lemonade-app.app in ${TAURI_APP_UNPACKED_DIR}"
        COMMENT "Copying Tauri App..."
        DEPENDS tauri-app
    )

    set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/cpack_prebuild.cmake")
    file(WRITE "${CMAKE_BINARY_DIR}/cpack_prebuild.cmake"
        "message(STATUS \"Pre-build: Ensuring Tauri app is prepared...\")\n"
        "execute_process(COMMAND ${CMAKE_COMMAND} --build \"${CMAKE_BINARY_DIR}\" --target prepare_tauri_app)\n"
    )

    # INSTALLATION (Component Structure)

    # Main Router -> /usr/local/bin
    install(TARGETS ${EXECUTABLE_NAME} RUNTIME DESTINATION "bin" COMPONENT Runtime)

    # Sign the router binary after installation (Release builds only)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        install(CODE "
            execute_process(
                COMMAND codesign --force --options runtime --timestamp --entitlements \"${ENTITLEMENTS_PATH}\" --sign \"${SIGNING_IDENTITY}\" -v \"${CMAKE_BINARY_DIR}/_CPack_Packages/Darwin/productbuild/Lemonade-${PROJECT_VERSION}-Darwin/Runtime/usr/local/bin/${EXECUTABLE_NAME}\"
                RESULT_VARIABLE SIGN_RESULT
            )
            if(NOT SIGN_RESULT EQUAL 0)
                message(FATAL_ERROR \"Failed to sign ${EXECUTABLE_NAME}\")
            endif()
        " COMPONENT Runtime)
    endif()

    # Tray client -> /usr/local/bin
    if(TARGET lemonade-tray)
        install(TARGETS lemonade-tray RUNTIME DESTINATION "bin" COMPONENT Runtime)

        # Sign the tray binary after installation (Release builds only)
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            install(CODE "
                execute_process(
                    COMMAND codesign --force --options runtime --timestamp --entitlements \"${ENTITLEMENTS_PATH}\" --sign \"${SIGNING_IDENTITY}\" -v \"${CMAKE_BINARY_DIR}/_CPack_Packages/Darwin/productbuild/Lemonade-${PROJECT_VERSION}-Darwin/Runtime/usr/local/bin/lemonade-tray\"
                    RESULT_VARIABLE SIGN_RESULT
                )
                if(NOT SIGN_RESULT EQUAL 0)
                    message(FATAL_ERROR \"Failed to sign lemonade-tray\")
                endif()
            " COMPONENT Runtime)
        endif()
    endif()

    # CLI -> /usr/local/bin
    if(TARGET lemonade)
        install(TARGETS lemonade RUNTIME DESTINATION "bin" COMPONENT Runtime)

        # Sign the CLI binary after installation (Release builds only)
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            install(CODE "
                execute_process(
                    COMMAND codesign --force --options runtime --timestamp --entitlements \"${ENTITLEMENTS_PATH}\" --sign \"${SIGNING_IDENTITY}\" -v \"${CMAKE_BINARY_DIR}/_CPack_Packages/Darwin/productbuild/Lemonade-${PROJECT_VERSION}-Darwin/Runtime/usr/local/bin/lemonade\"
                    RESULT_VARIABLE SIGN_RESULT
                )
                if(NOT SIGN_RESULT EQUAL 0)
                    message(FATAL_ERROR \"Failed to sign lemonade\")
                endif()
            " COMPONENT Runtime)
        endif()
    endif()

    # Resources -> /Library/Application Support/Lemonade/resources
    install(DIRECTORY ${CMAKE_BINARY_DIR}/resources/ DESTINATION "/Library/Application Support/Lemonade/resources" COMPONENT Resources)

    # App Bundle -> /Applications
    install(DIRECTORY "${TAURI_APP_BUILD_COPY}"
        DESTINATION "/Applications"
        USE_SOURCE_PERMISSIONS
        COMPONENT Applications
    )

    # Plists -> /Library/...
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ai.lemonadeserver.server.plist" DESTINATION "/Library/LaunchDaemons" COMPONENT Runtime)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ai.lemonadeserver.tray.plist" DESTINATION "/Library/LaunchAgents" COMPONENT Runtime)

    # Placeholder -> /usr/local/share/lemonade-server
    file(WRITE "${CMAKE_BINARY_DIR}/.keep" "")
    install(FILES "${CMAKE_BINARY_DIR}/.keep" DESTINATION "usr/local/share/lemonade-server" COMPONENT Runtime)

    # ackaging & Notarization
    set(CPACK_GENERATOR "productbuild")
    set(CPACK_PACKAGE_NAME "Lemonade")
    set(CPACK_PRODUCTBUILD_IDENTIFIER "ai.lemonadeserver.server")

    # Declare the package arm64-only so the macOS Installer does not assume our
    # postflight script needs x86_64 and prompt to install Rosetta on Apple Silicon.
    set(CPACK_APPLE_PKG_INSTALLER_CONTENT "<hostArchitectures><hostArchitecture>arm64</hostArchitecture></hostArchitectures>")

    set(CPACK_PRODUCTBUILD_DOMAIN "system")

    if(INSTALLER_IDENTITY)
        set(CPACK_PRODUCTBUILD_IDENTITY_NAME "${INSTALLER_IDENTITY}")
    endif()
    set(CPACK_PRODUCTBUILD_RELOCATE OFF)  # Prevent install_name_tool from invalidating signatures
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    set(CPACK_PRODUCTBUILD_BUNDLE ai.lemonadeserver.server.Applications OFF)
    set(CPACK_PRODUCTBUILD_BUNDLE_ID ai.lemonadeserver.server.Applications)

    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    set(CPACK_SET_DESTDIR ON)
    set(CPACK_PACKAGE_RELOCATABLE OFF)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_COMPONENT_APPLICATIONS_IS_ABSOLUTE ON)

    # license file extension for CPack
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

    set(CPACK_POSTFLIGHT_RUNTIME_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postinst-full-mac")
    include(CPack)

    set(PACKAGE_DEPENDS prepare_tauri_app ${EXECUTABLE_NAME})

    add_custom_target(package-macos
        DEPENDS ${PACKAGE_DEPENDS}
        COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG> --target package
        COMMENT "Running CPack to create signed installer..."
        VERBATIM
    )

    find_program(XCRUN_EXE xcrun)
    if(XCRUN_EXE)
        set(PKG_NAME "Lemonade-${PROJECT_VERSION}-Darwin.pkg")
        add_custom_target(notarize
            COMMAND ${CMAKE_COMMAND} -E echo "Submitting ${PKG_NAME} for notarization..."
            COMMAND ${XCRUN_EXE} notarytool submit "${CMAKE_BINARY_DIR}/${PKG_NAME}" --keychain-profile "AC_PASSWORD" --wait --verbose
            COMMAND ${CMAKE_COMMAND} -E echo "Stapling ticket to ${PKG_NAME}..."
            COMMAND ${XCRUN_EXE} stapler staple "${CMAKE_BINARY_DIR}/${PKG_NAME}"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Notarizing and Stapling macOS package..."
            VERBATIM
        )
        add_dependencies(notarize package-macos)
    endif()
endif()
