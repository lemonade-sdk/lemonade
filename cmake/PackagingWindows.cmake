# ============================================================
# WiX Toolset Configuration (Windows MSI Installer)
# ============================================================

# Configure WiX Product.wxs template with version
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/installer/Product.wxs.in
    ${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs
    @ONLY
)

set(WIX_PRODUCT_WXS "${CMAKE_CURRENT_BINARY_DIR}/installer/Product.wxs")
set(WIX_FRAGMENT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/installer/generate_tauri_fragment.py")

# Find WiX Toolset 5.0+ (unified 'wix' command) and Python for fragment generation
find_program(WIX_EXECUTABLE wix)
find_package(Python3 COMPONENTS Interpreter)

if(WIX_EXECUTABLE)
    execute_process(
        COMMAND ${WIX_EXECUTABLE} --version
        OUTPUT_VARIABLE WIX_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "WiX Toolset found: ${WIX_EXECUTABLE}")
    message(STATUS "WiX version: ${WIX_VERSION_OUTPUT}")

    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" WIX_SOURCE_DIR_NATIVE)
    file(TO_NATIVE_PATH "${WIX_PRODUCT_WXS}" WIX_PRODUCT_WXS_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lemonade-server-minimal.msi" WIX_MINIMAL_OUTPUT_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lemonade.msi" WIX_FULL_OUTPUT_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp" WIX_CPP_SOURCE_DIR_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" WIX_BUILD_DIR_NATIVE)
    file(TO_NATIVE_PATH "${TAURI_APP_UNPACKED_DIR}" WIX_TAURI_SOURCE_NATIVE)
    set(WIX_TAURI_FRAGMENT "${CMAKE_CURRENT_BINARY_DIR}/installer/TauriAppFragment.wxs")
    file(TO_NATIVE_PATH "${WIX_TAURI_FRAGMENT}" WIX_TAURI_FRAGMENT_NATIVE)


    # Request the extension matching the CLI that was actually found, so
    # `wix build` cannot resolve a newer major from NuGet that this CLI then
    # refuses to load. Derived rather than hardcoded: the project supports
    # WiX 5.0+, so pinning a literal would break a newer supported CLI.
    # Falls back to unversioned if the banner is not parseable.
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" WIX_EXT_VERSION "${WIX_VERSION_OUTPUT}")
    if(WIX_EXT_VERSION)
        set(WIX_UI_EXTENSION "WixToolset.UI.wixext/${WIX_EXT_VERSION}")
    else()
        set(WIX_UI_EXTENSION "WixToolset.UI.wixext")
    endif()
    message(STATUS "WiX UI extension: ${WIX_UI_EXTENSION}")

    # Web app fragment paths. The minimal and full installers each get
    # their own file: both regenerate it and then feed it to wix build, so
    # a shared path makes the two targets race whenever a build runs them
    # in parallel (`cmake --build --target wix_installers -- /m`).
    # Their wix intermediate folders are separated below for the same
    # reason — both compile Product.wxs, so a shared obj/ would collide on
    # Product.wixobj with different -d IncludeTauriApp values.
    set(WIX_INTERMEDIATE_MINIMAL "${CMAKE_CURRENT_BINARY_DIR}/installer/obj-minimal")
    file(TO_NATIVE_PATH "${WIX_INTERMEDIATE_MINIMAL}" WIX_INTERMEDIATE_MINIMAL_NATIVE)
    set(WIX_INTERMEDIATE_FULL "${CMAKE_CURRENT_BINARY_DIR}/installer/obj-full")
    file(TO_NATIVE_PATH "${WIX_INTERMEDIATE_FULL}" WIX_INTERMEDIATE_FULL_NATIVE)
    file(TO_NATIVE_PATH "${WEB_APP_BUILD_DIR}" WIX_WEBAPP_SOURCE_NATIVE)
    set(WIX_WEBAPP_FRAGMENT_MINIMAL "${CMAKE_CURRENT_BINARY_DIR}/installer/WebAppFragment.minimal.wxs")
    file(TO_NATIVE_PATH "${WIX_WEBAPP_FRAGMENT_MINIMAL}" WIX_WEBAPP_FRAGMENT_MINIMAL_NATIVE)
    set(WIX_WEBAPP_FRAGMENT_FULL "${CMAKE_CURRENT_BINARY_DIR}/installer/WebAppFragment.full.wxs")
    file(TO_NATIVE_PATH "${WIX_WEBAPP_FRAGMENT_FULL}" WIX_WEBAPP_FRAGMENT_FULL_NATIVE)

    # Both installers require Python3 for fragment generation and always include the web app
    if(NOT Python3_FOUND)
        message(FATAL_ERROR "Python 3 is required for WiX installer fragment generation. Install Python 3 and ensure it is in PATH.")
    endif()

    # Minimal installer (server + web-app)
    add_custom_target(wix_installer_minimal
        COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (server + web-app)..."
        # Generate web-app fragment
        COMMAND ${Python3_EXECUTABLE} ${WIX_FRAGMENT_SCRIPT}
            --source "${WEB_APP_BUILD_DIR}"
            --output "${WIX_WEBAPP_FRAGMENT_MINIMAL}"
            --component-group "WebAppComponents"
            --root-id "WebAppDir"
            --path-variable "WebAppSourceDir"
        COMMAND ${WIX_EXECUTABLE} build
            -arch x64
            -intermediateFolder "${WIX_INTERMEDIATE_MINIMAL_NATIVE}"
            -ext ${WIX_UI_EXTENSION}
            -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
            -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
            -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
            -d IncludeTauriApp=0
            -d IncludeWebApp=1
            -d WebAppSourceDir="${WIX_WEBAPP_SOURCE_NATIVE}"
            -out "${WIX_MINIMAL_OUTPUT_NATIVE}"
            "${WIX_PRODUCT_WXS_NATIVE}"
            "${WIX_WEBAPP_FRAGMENT_MINIMAL_NATIVE}"
        COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade-server-minimal.msi"
        DEPENDS ${EXECUTABLE_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building WiX MSI installer (server + web-app)"
    )

    set(WIX_INSTALLER_TARGETS wix_installer_minimal)

    # Full installer (server + Tauri app + web-app)
    add_custom_target(wix_installer_full
        COMMAND ${CMAKE_COMMAND} -E echo "Building WiX MSI installer (with Tauri app)..."
        # Generate web-app fragment
        COMMAND ${Python3_EXECUTABLE} ${WIX_FRAGMENT_SCRIPT}
            --source "${WEB_APP_BUILD_DIR}"
            --output "${WIX_WEBAPP_FRAGMENT_FULL}"
            --component-group "WebAppComponents"
            --root-id "WebAppDir"
            --path-variable "WebAppSourceDir"
        # Generate Tauri app fragment
        COMMAND ${Python3_EXECUTABLE} ${WIX_FRAGMENT_SCRIPT}
            --source "${TAURI_APP_UNPACKED_DIR}"
            --output "${WIX_TAURI_FRAGMENT}"
            --component-group "TauriAppComponents"
            --root-id "TauriAppDir"
            --path-variable "TauriSourceDir"
        COMMAND ${WIX_EXECUTABLE} build
            -arch x64
            -intermediateFolder "${WIX_INTERMEDIATE_FULL_NATIVE}"
            -ext ${WIX_UI_EXTENSION}
            -d SourceDir="${WIX_SOURCE_DIR_NATIVE}"
            -d CppSourceDir="${WIX_CPP_SOURCE_DIR_NATIVE}"
            -d BuildDir="${WIX_BUILD_DIR_NATIVE}"
            -d IncludeTauriApp=1
            -d IncludeWebApp=1
            -d TauriSourceDir="${WIX_TAURI_SOURCE_NATIVE}"
            -d WebAppSourceDir="${WIX_WEBAPP_SOURCE_NATIVE}"
            -out "${WIX_FULL_OUTPUT_NATIVE}"
            "${WIX_PRODUCT_WXS_NATIVE}"
            "${WIX_TAURI_FRAGMENT_NATIVE}"
            "${WIX_WEBAPP_FRAGMENT_FULL_NATIVE}"
        COMMAND ${CMAKE_COMMAND} -E echo "MSI installer created: lemonade.msi"
        DEPENDS ${EXECUTABLE_NAME}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building WiX MSI installer (with Tauri app)"
    )

    list(APPEND WIX_INSTALLER_TARGETS wix_installer_full)

    add_custom_target(wix_installers
        DEPENDS ${WIX_INSTALLER_TARGETS}
    )

    message(STATUS "WiX installer targets configured. Run 'cmake --build . --target wix_installer_minimal' or 'wix_installer_full'.")
else()
    message(STATUS "WiX Toolset not found. MSI installers will not be available.")
    message(STATUS "  Install WiX Toolset 5.0.2 from: https://github.com/wixtoolset/wix/releases/download/v5.0.2/wix-cli-x64.msi")
    message(STATUS "  Or visit: https://wixtoolset.org/")
endif()
