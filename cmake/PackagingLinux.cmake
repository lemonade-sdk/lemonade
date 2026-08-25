# ============================================================
# CPack Configuration (Linux only)
# ============================================================
if(UNIX AND NOT APPLE)
    set(CPACK_SET_DESTDIR ON)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Lemonade")
    set(CPACK_PACKAGE_CONTACT "lemonade@amd.com")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lemonade Local LLM Server")
    set(CPACK_PACKAGE_DESCRIPTION "A lightweight, high-performance local LLM server with support for multiple backends including llama.cpp, FastFlowLM, and RyzenAI.")

    # Only install our executables (not curl/development files)
    install(TARGETS ${EXECUTABLE_NAME}
        RUNTIME DESTINATION bin
    )

    # Install resources (all files including KaTeX fonts)
    install(DIRECTORY ${CMAKE_BINARY_DIR}/resources/
        DESTINATION share/lemonade-server/resources
    )

    # Install packaged config defaults for distro-managed overrides.
    install(FILES ${CMAKE_BINARY_DIR}/resources/defaults.json
        DESTINATION share/lemonade
    )

    # Install example scripts
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/examples/
        DESTINATION share/lemonade-server/examples
        USE_SOURCE_PERMISSIONS
        PATTERN "*.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )

    # Helper: create a systemd unit symlink with fatal-error-on-failure semantics
    function(lemonade_install_systemd_symlink link_path target_path)
        get_filename_component(_link_dir "${link_path}" DIRECTORY)

        install(CODE "
            set(_link \"\$ENV{DESTDIR}${link_path}\")
            set(_target \"${target_path}\")

            file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${_link_dir}\")

            if(IS_SYMLINK \"\${_link}\")
                file(REMOVE \"\${_link}\")
            elseif(EXISTS \"\${_link}\")
                message(FATAL_ERROR \"Refusing to replace existing non-symlink: \${_link}\")
            endif()

            execute_process(
                COMMAND \"${CMAKE_COMMAND}\" -E create_symlink \"\${_target}\" \"\${_link}\"
                RESULT_VARIABLE _result
                ERROR_VARIABLE _error
            )

            if(NOT _result EQUAL 0)
                message(FATAL_ERROR \"Failed to create systemd symlink \${_link} -> \${_target}: \${_error}\")
            endif()
        ")
    endfunction()

    # Configure systemd service file (uses CMAKE_INSTALL_FULL_* variables)
    configure_file(
        ${CMAKE_SOURCE_DIR}/data/lemond.service.in
        ${CMAKE_BINARY_DIR}/lemond.service
        @ONLY
    )

    # Install systemd service file
    install(FILES ${CMAKE_BINARY_DIR}/lemond.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system
    )
    # Create symlinks in standard systemd search paths only if not installing to /usr
    # (if prefix is /usr, the service is already in the standard location)
    if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
        lemonade_install_systemd_symlink(
            "/usr/lib/systemd/system/lemond.service"
            "${CMAKE_INSTALL_PREFIX}/lib/systemd/system/lemond.service"
        )
        lemonade_install_systemd_symlink(
            "/usr/lib/systemd/user/lemond.service"
            "${CMAKE_INSTALL_PREFIX}/lib/systemd/user/lemond.service"
        )
    endif()

    # Install sysusers.d snippet so the 'lemonade' system user is created
    install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade.sysusers
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/sysusers.d
        RENAME lemonade.conf
    )

    # Configure + install the user-mode systemd unit
    configure_file(
        ${CMAKE_SOURCE_DIR}/data/lemond-user.service.in
        ${CMAKE_BINARY_DIR}/lemond-user.service
        @ONLY
    )
    install(FILES ${CMAKE_BINARY_DIR}/lemond-user.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user
        RENAME lemond.service
    )

    # Install the optional environment-file template loaded by the unit via
    # EnvironmentFile=-/etc/default/lemond (LEMONADE_API_KEY and other secrets).
    install(FILES ${CMAKE_SOURCE_DIR}/data/secrets.conf
        DESTINATION /etc/default
        RENAME lemond
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ
    )

    # Create symlinks for KaTeX fonts if they exist on the system
    # Check if fonts-katex package fonts are available
    set(KATEX_FONTS_DIR "/usr/share/fonts/truetype/katex")
    if(EXISTS "${KATEX_FONTS_DIR}")
        message(STATUS "KaTeX fonts found at ${KATEX_FONTS_DIR}, creating symlinks")

        # Ensure web-app directory exists in build directory
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/resources/web-app")

        set(KATEX_FONTS
            "KaTeX_AMS-Regular.ttf" "KaTeX_AMS-Regular.woff" "KaTeX_AMS-Regular.woff2"
            "KaTeX_Caligraphic-Bold.ttf" "KaTeX_Caligraphic-Bold.woff" "KaTeX_Caligraphic-Bold.woff2"
            "KaTeX_Caligraphic-Regular.ttf" "KaTeX_Caligraphic-Regular.woff" "KaTeX_Caligraphic-Regular.woff2"
            "KaTeX_Fraktur-Bold.ttf" "KaTeX_Fraktur-Bold.woff" "KaTeX_Fraktur-Bold.woff2"
            "KaTeX_Fraktur-Regular.ttf" "KaTeX_Fraktur-Regular.woff" "KaTeX_Fraktur-Regular.woff2"
            "KaTeX_Main-Bold.ttf" "KaTeX_Main-Bold.woff" "KaTeX_Main-Bold.woff2"
            "KaTeX_Main-BoldItalic.ttf" "KaTeX_Main-BoldItalic.woff" "KaTeX_Main-BoldItalic.woff2"
            "KaTeX_Main-Italic.ttf" "KaTeX_Main-Italic.woff" "KaTeX_Main-Italic.woff2"
            "KaTeX_Main-Regular.ttf" "KaTeX_Main-Regular.woff" "KaTeX_Main-Regular.woff2"
            "KaTeX_Math-BoldItalic.ttf" "KaTeX_Math-BoldItalic.woff" "KaTeX_Math-BoldItalic.woff2"
            "KaTeX_Math-Italic.ttf" "KaTeX_Math-Italic.woff" "KaTeX_Math-Italic.woff2"
            "KaTeX_SansSerif-Bold.ttf" "KaTeX_SansSerif-Bold.woff" "KaTeX_SansSerif-Bold.woff2"
            "KaTeX_SansSerif-Italic.ttf" "KaTeX_SansSerif-Italic.woff" "KaTeX_SansSerif-Italic.woff2"
            "KaTeX_SansSerif-Regular.ttf" "KaTeX_SansSerif-Regular.woff" "KaTeX_SansSerif-Regular.woff2"
            "KaTeX_Script-Regular.ttf" "KaTeX_Script-Regular.woff" "KaTeX_Script-Regular.woff2"
            "KaTeX_Size1-Regular.ttf" "KaTeX_Size1-Regular.woff" "KaTeX_Size1-Regular.woff2"
            "KaTeX_Size2-Regular.ttf" "KaTeX_Size2-Regular.woff" "KaTeX_Size2-Regular.woff2"
            "KaTeX_Size3-Regular.ttf" "KaTeX_Size3-Regular.woff" "KaTeX_Size3-Regular.woff2"
            "KaTeX_Size4-Regular.ttf" "KaTeX_Size4-Regular.woff" "KaTeX_Size4-Regular.woff2"
            "KaTeX_Typewriter-Regular.ttf" "KaTeX_Typewriter-Regular.woff" "KaTeX_Typewriter-Regular.woff2"
        )

        foreach(font ${KATEX_FONTS})
            set(FONT_LINK "${CMAKE_BINARY_DIR}/resources/web-app/${font}")
            set(FONT_TARGET "${KATEX_FONTS_DIR}/${font}")
            # Only create symlink if the target font exists
            if(EXISTS "${FONT_TARGET}")
                # Remove existing file/symlink if present
                if(EXISTS "${FONT_LINK}" OR IS_SYMLINK "${FONT_LINK}")
                    file(REMOVE "${FONT_LINK}")
                endif()
                # Create symlink in build directory
                file(CREATE_LINK "${FONT_TARGET}" "${FONT_LINK}" SYMBOLIC)
            endif()
        endforeach()
    else()
        message(STATUS "KaTeX fonts not found at ${KATEX_FONTS_DIR}, using bundled fonts")
    endif()

    # Check if Tauri app is available for full package
    option(BUILD_TAURI_APP "Build and include Tauri desktop app in deb package" OFF)

    # Build Tauri app if requested and target is available
    if(BUILD_TAURI_APP AND TARGET tauri-app)
        message(STATUS "BUILD_TAURI_APP is ON - building Tauri app...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tauri-app
            RESULT_VARIABLE TAURI_BUILD_RESULT
        )
        if(NOT TAURI_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build Tauri app")
        endif()
    endif()

    if(BUILD_TAURI_APP AND EXISTS "${TAURI_APP_UNPACKED_DIR}/${TAURI_EXE_NAME}")
        message(STATUS "Tauri app found at ${TAURI_APP_UNPACKED_DIR}")
        message(STATUS "Building full deb package with Tauri app")

        # Full package name
        set(CPACK_PACKAGE_NAME "lemonade-server")

        # Install the Tauri app binary (single file, unlike Electron's directory tree)
        install(PROGRAMS "${TAURI_APP_UNPACKED_DIR}/${TAURI_EXE_NAME}"
            DESTINATION share/lemonade-server/app
        )

        if (NOT WIN32 AND NOT APPLE)
            # Install .desktop file for the Tauri app
            install(FILES ${CMAKE_SOURCE_DIR}/data/lemonade-app.desktop
                DESTINATION share/applications
            )
            # Install MetaInfo file for the Tauri app
            install(FILES ${CMAKE_SOURCE_DIR}/data/ai.lemonadeserver.app.metainfo.xml
                DESTINATION share/metainfo
            )
            # Create symlink in standard applications path only if not installing to /usr
            if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
                install(CODE "
                    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/share/applications\")
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} -E create_symlink
                            ${CMAKE_INSTALL_PREFIX}/share/applications/lemonade-app.desktop
                            \"\$ENV{DESTDIR}/usr/share/applications/lemonade-app.desktop\"
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
            # Create symlink in standard bin path only if not installing to /usr
            if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
                install(CODE "
                    file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} -E create_symlink
                            \"\${CMAKE_INSTALL_PREFIX}/share/lemonade-server/app/lemonade-app\"
                            \"\$ENV{DESTDIR}/usr/bin/lemonade-app\"
                    )
                ")
            else()
                install(CODE "
                    file(CREATE_LINK
                        \"\${CMAKE_INSTALL_PREFIX}/share/lemonade-server/app/lemonade-app\"
                        \"\${CMAKE_INSTALL_PREFIX}/bin/lemonade-app\"
                        SYMBOLIC)
                ")
            endif()
        endif()

    else()
        set(CPACK_PACKAGE_NAME "lemonade-server")
    endif()

    # RPM specific variables defined within
    include(${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/CPackRPM.cmake)
    include(CPack)
endif()
