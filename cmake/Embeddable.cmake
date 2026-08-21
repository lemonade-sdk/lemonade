# ============================================================
# Embeddable Archive Target (Cross-Platform)
# ============================================================
# Builds lemond + lemonade, assembles a portable archive with
# binaries, LICENSE, and resource files. Produces .tar.gz on
# Linux or .zip on Windows.
#
# Usage:
#   cmake --preset default -DBUILD_WEB_APP=OFF
#   cmake --build --preset default --target embeddable

if(WIN32)
    set(EMBEDDABLE_PLATFORM "windows-x64")
elseif(APPLE)
    if(DEFINED CMAKE_OSX_ARCHITECTURES AND CMAKE_OSX_ARCHITECTURES)
        set(_embeddable_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_embeddable_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    if(_embeddable_arch STREQUAL "arm64")
        set(EMBEDDABLE_PLATFORM "macos-arm64")
    else()
        set(EMBEDDABLE_PLATFORM "macos-x64")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(EMBEDDABLE_PLATFORM "ubuntu-arm64")
    else()
        set(EMBEDDABLE_PLATFORM "ubuntu-x64")
    endif()
endif()

set(EMBEDDABLE_DIR_NAME "lemonade-embeddable-${PROJECT_VERSION}-${EMBEDDABLE_PLATFORM}")
set(EMBEDDABLE_STAGING_DIR "${CMAKE_BINARY_DIR}/${EMBEDDABLE_DIR_NAME}")

if(WIN32)
    set(EMBEDDABLE_ARCHIVE_NAME "${EMBEDDABLE_DIR_NAME}.zip")
    set(EMBEDDABLE_TAR_CMD ${CMAKE_COMMAND} -E tar cf
        "${CMAKE_BINARY_DIR}/${EMBEDDABLE_ARCHIVE_NAME}" --format=zip
        "${EMBEDDABLE_DIR_NAME}")
else()
    set(EMBEDDABLE_ARCHIVE_NAME "${EMBEDDABLE_DIR_NAME}.tar.gz")
    set(EMBEDDABLE_TAR_CMD ${CMAKE_COMMAND} -E tar czf
        "${CMAKE_BINARY_DIR}/${EMBEDDABLE_ARCHIVE_NAME}"
        "${EMBEDDABLE_DIR_NAME}")
endif()

add_custom_target(embeddable
    # Clean and create staging directory
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${EMBEDDABLE_STAGING_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EMBEDDABLE_STAGING_DIR}/resources"

    # Copy binaries
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:lemond>" "${EMBEDDABLE_STAGING_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:lemonade>" "${EMBEDDABLE_STAGING_DIR}/"

    # Copy LICENSE
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${EMBEDDABLE_STAGING_DIR}/"

    # Copy resource files
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:lemond>/resources/server_models.json"
        "${EMBEDDABLE_STAGING_DIR}/resources/"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:lemond>/resources/backend_versions.json"
        "${EMBEDDABLE_STAGING_DIR}/resources/"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:lemond>/resources/vllm_model_config.json"
        "${EMBEDDABLE_STAGING_DIR}/resources/"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:lemond>/resources/defaults.json"
        "${EMBEDDABLE_STAGING_DIR}/resources/"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:lemond>/resources/bench_scenarios.json"
        "${EMBEDDABLE_STAGING_DIR}/resources/"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE_DIR:lemond>/resources/toolDefinitions.json"
        "${EMBEDDABLE_STAGING_DIR}/resources/"

    # Create archive
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}" ${EMBEDDABLE_TAR_CMD}

    DEPENDS lemond lemonade
    COMMENT "Assembling embeddable archive: ${EMBEDDABLE_ARCHIVE_NAME}"
    VERBATIM
)

message(STATUS "Embeddable archive target available: cmake --build . --target embeddable")
message(STATUS "  Archive: ${EMBEDDABLE_ARCHIVE_NAME}")
