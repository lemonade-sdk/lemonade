# Cross-platform script to build the web app
# Usage: cmake -DAPP_SOURCE_DIR=... -DWEB_APP_SOURCE_DIR=... -DWEB_APP_BUILD_STAGING_DIR=... -DWEB_APP_BUILD_DIR=... -DNPM_EXECUTABLE=... -DWEB_APP_STAMP=... -DUSE_SYSTEM_NODEJS_MODULES=ON -P BuildWebApp.cmake
#
# Staging layout:
#   ${WEB_APP_BUILD_STAGING_DIR}/
#     app/        ← copy of src/app/ (the shared React renderer source)
#     web-app/    ← copy of src/web-app/ (this build's package.json + webpack config)
#
# Webpack runs from staging/web-app/ and resolves the shared sources via
# `../app/src/...`. We deliberately do NOT use OS symlinks for the shared
# source — those break Windows checkouts unless `core.symlinks=true` and
# developer mode are both enabled.

option(USE_SYSTEM_NODEJS_MODULES "Use system nodejs modules and fonts" ON)
string(TOUPPER "${USE_SYSTEM_NODEJS_MODULES}" USE_SYSTEM_NODEJS_MODULES_UPPER)
set(USE_SYSTEM_NODEJS_MODULES_ENABLED OFF)
if(USE_SYSTEM_NODEJS_MODULES_UPPER STREQUAL "ON" OR USE_SYSTEM_NODEJS_MODULES_UPPER STREQUAL "TRUE" OR USE_SYSTEM_NODEJS_MODULES_UPPER STREQUAL "1")
    set(USE_SYSTEM_NODEJS_MODULES_ENABLED ON)
endif()

message(STATUS "Building Web app...")

# Staging paths: app/ and web-app/ as siblings inside WEB_APP_BUILD_STAGING_DIR
set(STAGED_APP_DIR "${WEB_APP_BUILD_STAGING_DIR}/app")
set(STAGED_WEB_APP_DIR "${WEB_APP_BUILD_STAGING_DIR}/web-app")

# Remove staging dir if it exists to ensure a clean copy
if(EXISTS "${WEB_APP_BUILD_STAGING_DIR}")
    file(REMOVE_RECURSE "${WEB_APP_BUILD_STAGING_DIR}")
endif()
file(MAKE_DIRECTORY "${WEB_APP_BUILD_STAGING_DIR}")

# Copy a directory cross-platform, dereferencing any symlinks the source tree
# happens to contain (e.g. src/app/assets/favicon.ico is a symlink into the
# docs site to dedupe the icon). robocopy follows symlinks by default; cp -L
# is the equivalent on Unix.
function(_stage_dir SRC DST)
    if(WIN32)
        # robocopy returns 0-7 on success/warnings, 8+ on actual errors
        execute_process(
            COMMAND robocopy "${SRC}" "${DST}" /E /NFL /NDL
            RESULT_VARIABLE COPY_RESULT
        )
        if(COPY_RESULT GREATER 7)
            message(FATAL_ERROR "Failed to stage ${SRC} → ${DST} (robocopy exit code ${COPY_RESULT})")
        endif()
    else()
        execute_process(
            COMMAND cp -rL "${SRC}" "${DST}"
            RESULT_VARIABLE COPY_RESULT
        )
        if(NOT COPY_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to stage ${SRC} → ${DST} (exit code ${COPY_RESULT})")
        endif()
    endif()
endfunction()

message(STATUS "Staging shared app sources from ${APP_SOURCE_DIR} to ${STAGED_APP_DIR}")
_stage_dir("${APP_SOURCE_DIR}" "${STAGED_APP_DIR}")

message(STATUS "Staging web-app sources from ${WEB_APP_SOURCE_DIR} to ${STAGED_WEB_APP_DIR}")
_stage_dir("${WEB_APP_SOURCE_DIR}" "${STAGED_WEB_APP_DIR}")

# Backward-compatible alias used by the rest of this script: webpack runs in
# the staged web-app directory, which now sits at staging/web-app/.
set(WEB_APP_BUILD_SOURCE_DIR "${STAGED_WEB_APP_DIR}")

# System nodejs modules and KaTeX fonts integration
if(USE_SYSTEM_NODEJS_MODULES_ENABLED)
    set(SYSTEM_NODE_MODULES "/usr/share/nodejs")
    set(SYSTEM_KATEX_JS "/usr/share/javascript/katex/katex.js")
    set(SYSTEM_KATEX_CSS "/usr/share/javascript/katex/katex.min.css")
    set(SYSTEM_KATEX_FONTS "/usr/share/fonts/truetype/katex")
    set(OVERLAY_DIR "${WEB_APP_BUILD_SOURCE_DIR}/katex-overlay")

    # Set up katex overlay to redirect to system katex (only if available)
    if(EXISTS "${SYSTEM_KATEX_JS}" AND EXISTS "${SYSTEM_KATEX_CSS}")
        message(STATUS "Setting up katex overlay from system packages at ${SYSTEM_KATEX_JS}")
        file(MAKE_DIRECTORY "${OVERLAY_DIR}/katex/dist")

        # Create the katex shim that redirects to system katex
        file(WRITE "${OVERLAY_DIR}/katex/index.js" "module.exports = require('${SYSTEM_KATEX_JS}');\n")

        # Create a symlink to the system CSS file so webpack can find it at the expected path
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink "${SYSTEM_KATEX_CSS}" "${OVERLAY_DIR}/katex/dist/katex.min.css"
            RESULT_VARIABLE SYMLINK_RESULT
        )
        if(NOT SYMLINK_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to create KaTeX CSS symlink (exit code ${SYMLINK_RESULT})")
        endif()
    else()
        message(STATUS "System katex not found - building without katex overlay")
    endif()

    if(EXISTS "${SYSTEM_KATEX_FONTS}")
        message(STATUS "System KaTeX fonts available at ${SYSTEM_KATEX_FONTS}")
    else()
        message(FATAL_ERROR "System KaTeX fonts not found at ${SYSTEM_KATEX_FONTS} while USE_SYSTEM_NODEJS_MODULES is enabled")
    endif()
endif()

# Check if we have npm or webpack available
if(NOT WEBPACK_EXECUTABLE)
    find_program(WEBPACK_EXECUTABLE webpack)
endif()

# Install dependencies only in non-system mode
if(NOT USE_SYSTEM_NODEJS_MODULES_ENABLED)
    if(NOT NPM_EXECUTABLE)
        message(FATAL_ERROR "npm not found - non-system web app build requires npm")
    endif()

    message(STATUS "Installing npm dependencies...")
    execute_process(
        COMMAND "${NPM_EXECUTABLE}" ci --ignore-scripts
        WORKING_DIRECTORY "${WEB_APP_BUILD_SOURCE_DIR}"
        RESULT_VARIABLE INSTALL_RESULT
    )

    if(NOT INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR "Web app npm install failed with exit code ${INSTALL_RESULT}")
    endif()
endif()

# Set the environment variable for webpack output path
set(ENV{WEBPACK_OUTPUT_PATH} "${WEB_APP_BUILD_DIR}")
if(USE_SYSTEM_NODEJS_MODULES_ENABLED)
    set(ENV{USE_SYSTEM_NODEJS_MODULES} "1")
else()
    set(ENV{USE_SYSTEM_NODEJS_MODULES} "0")
endif()

# Set NODE_PATH for system packages with overlay
if(USE_SYSTEM_NODEJS_MODULES_ENABLED)
    set(OVERLAY_KATEX_DIR "${WEB_APP_BUILD_SOURCE_DIR}/katex-overlay")
    set(NODE_PATH_VALUE "${OVERLAY_KATEX_DIR}:/usr/share/nodejs:/usr/lib/nodejs:/usr/share/javascript")
    set(ENV{NODE_PATH} "${NODE_PATH_VALUE}")
    message(STATUS "Using NODE_PATH: ${NODE_PATH_VALUE}")
endif()

# Execute build - use webpack directly if available, otherwise use npm
if(USE_SYSTEM_NODEJS_MODULES_ENABLED)
    if(NOT WEBPACK_EXECUTABLE)
        message(FATAL_ERROR "webpack not found - USE_SYSTEM_NODEJS_MODULES=ON requires system webpack")
    endif()

    message(STATUS "Running webpack directly with output to ${WEB_APP_BUILD_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
            "NODE_PATH=${NODE_PATH_VALUE}"
            "WEBPACK_OUTPUT_PATH=${WEB_APP_BUILD_DIR}"
            "USE_SYSTEM_NODEJS_MODULES=1"
            "${WEBPACK_EXECUTABLE}" --mode production
        WORKING_DIRECTORY "${WEB_APP_BUILD_SOURCE_DIR}"
        RESULT_VARIABLE BUILD_RESULT
    )
elseif(NPM_EXECUTABLE)
    message(STATUS "Running npm build with output to ${WEB_APP_BUILD_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
            "WEBPACK_OUTPUT_PATH=${WEB_APP_BUILD_DIR}"
            "${NPM_EXECUTABLE}" run build
        WORKING_DIRECTORY "${WEB_APP_BUILD_SOURCE_DIR}"
        RESULT_VARIABLE BUILD_RESULT
    )
else()
    message(FATAL_ERROR "Neither webpack nor npm found - cannot build web app")
endif()

if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Web app build failed with exit code ${BUILD_RESULT}")
endif()

# Create stamp file to mark successful build
file(TOUCH "${WEB_APP_STAMP}")
message(STATUS "Web app build completed successfully")
