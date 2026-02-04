# Cross-platform script to build the web app
# Usage: cmake -DWEB_APP_SOURCE_DIR=... -DWEB_APP_BUILD_SOURCE_DIR=... -DWEB_APP_BUILD_DIR=... -DNPM_EXECUTABLE=... -DWEB_APP_STAMP=... -P BuildWebApp.cmake

message(STATUS "Building Web app...")

# Copy source files to build directory, following symlinks
message(STATUS "Copying Web app sources from ${WEB_APP_SOURCE_DIR} to ${WEB_APP_BUILD_SOURCE_DIR}")

# Remove destination if it exists to ensure clean copy
if(EXISTS "${WEB_APP_BUILD_SOURCE_DIR}")
    file(REMOVE_RECURSE "${WEB_APP_BUILD_SOURCE_DIR}")
endif()

# Use platform-specific copy command with symlink following
if(WIN32)
    # Windows: robocopy with /E (recurse empty dirs) /NFL (no file list) /NDL (no directory list)
    # robocopy returns 0-3 for success, 4-7 for warnings, 8+ for errors
    execute_process(
        COMMAND robocopy "${WEB_APP_SOURCE_DIR}" "${WEB_APP_BUILD_SOURCE_DIR}" /E /NFL /NDL
        RESULT_VARIABLE COPY_RESULT
    )
    # Check for actual errors (exit codes 8 and higher indicate errors)
    if(COPY_RESULT GREATER 7)
        message(FATAL_ERROR "Failed to copy Web app sources (robocopy exit code ${COPY_RESULT})")
    endif()
else()
    # Unix/Linux: cp with -rL (recursive, dereference symlinks)
    execute_process(
        COMMAND cp -rL "${WEB_APP_SOURCE_DIR}" "${WEB_APP_BUILD_SOURCE_DIR}"
        RESULT_VARIABLE COPY_RESULT
    )
    if(NOT COPY_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to copy Web app sources (exit code ${COPY_RESULT})")
    endif()
endif()

# Install dependencies
message(STATUS "Installing npm dependencies...")
execute_process(
    COMMAND "${NPM_EXECUTABLE}" install
    WORKING_DIRECTORY "${WEB_APP_BUILD_SOURCE_DIR}"
    RESULT_VARIABLE INSTALL_RESULT
)

if(NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "Web app npm install failed with exit code ${INSTALL_RESULT}")
endif()

# Set the environment variable for webpack output path
set(ENV{WEBPACK_OUTPUT_PATH} "${WEB_APP_BUILD_DIR}")

# Execute npm build in the build directory
message(STATUS "Running npm build with output to ${WEB_APP_BUILD_DIR}")
execute_process(
    COMMAND "${NPM_EXECUTABLE}" run build
    WORKING_DIRECTORY "${WEB_APP_BUILD_SOURCE_DIR}"
    RESULT_VARIABLE BUILD_RESULT
)

if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Web app build failed with exit code ${BUILD_RESULT}")
endif()

# Create stamp file to mark successful build
file(TOUCH "${WEB_APP_STAMP}")
message(STATUS "Web app build completed successfully")
