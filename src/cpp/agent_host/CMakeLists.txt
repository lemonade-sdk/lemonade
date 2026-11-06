# lemonade-agent-host: the per-user process that installs, configures and
# launches agents for lemond. Like the CLI, it compiles the server utilities
# and the CLI's pi sources in place instead of linking lemonade-server-core.

set(LEMON_AGENT_HOST_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_installer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_paths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_pins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_shim.cpp
)
set(LEMON_AGENT_HOST_CORE_SOURCES ${LEMON_AGENT_HOST_CORE_SOURCES} PARENT_SCOPE)

set(_AGENT_HOST_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../server)
set(_AGENT_HOST_CLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../cli)
set(LEMON_AGENT_HOST_SOURCES
    ${LEMON_AGENT_HOST_CORE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/host_link.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host_routes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pi_driver.cpp
    ${_AGENT_HOST_CLI_DIR}/agent_config_file.cpp
    ${_AGENT_HOST_CLI_DIR}/agent_launcher.cpp
    ${_AGENT_HOST_CLI_DIR}/agent_models.cpp
    ${_AGENT_HOST_CLI_DIR}/pi_launch_prep.cpp
    ${_AGENT_HOST_CLI_DIR}/pi_profile.cpp
    ${_AGENT_HOST_SERVER_DIR}/utils/http_client.cpp
    ${_AGENT_HOST_SERVER_DIR}/utils/json_utils.cpp
    ${_AGENT_HOST_SERVER_DIR}/utils/path_utils.cpp
    ${_AGENT_HOST_SERVER_DIR}/utils/process_manager.cpp
    ${_AGENT_HOST_SERVER_DIR}/utils/url_utils.cpp
)
if(WIN32)
    list(APPEND LEMON_AGENT_HOST_SOURCES
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/archive_windows.cpp
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/path_windows.cpp
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/process_windows.cpp)
elseif(APPLE)
    list(APPEND LEMON_AGENT_HOST_SOURCES
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/archive_unix.cpp
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/path_macos.cpp
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/process_macos.cpp)
else()
    list(APPEND LEMON_AGENT_HOST_SOURCES
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/archive_unix.cpp
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/path_linux.cpp
        ${_AGENT_HOST_SERVER_DIR}/utils/platform/process_linux.cpp)
endif()

add_executable(lemonade-agent-host ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${LEMON_AGENT_HOST_SOURCES})
target_include_directories(lemonade-agent-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_BINARY_DIR}/include
)
target_link_libraries(lemonade-agent-host PRIVATE
    nlohmann_json::nlohmann_json
    lemonade-digest-crypto
    lemonade-httplib
)
if(USE_SYSTEM_CURL)
    target_link_libraries(lemonade-agent-host PRIVATE ${CURL_LIBRARIES})
    target_include_directories(lemonade-agent-host PRIVATE ${CURL_INCLUDE_DIRS})
    target_compile_options(lemonade-agent-host PRIVATE ${CURL_CFLAGS_OTHER})
else()
    target_link_libraries(lemonade-agent-host PRIVATE libcurl)
endif()
if(USE_SYSTEM_ZSTD)
    target_link_libraries(lemonade-agent-host PRIVATE ${ZSTD_LIBRARIES})
    target_link_directories(lemonade-agent-host PRIVATE ${ZSTD_LIBRARY_DIRS})
endif()
set_target_properties(lemonade-agent-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    set_target_properties(lemonade-agent-host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release")
    target_compile_definitions(lemonade-agent-host PRIVATE UNICODE _UNICODE)
    target_link_libraries(lemonade-agent-host PRIVATE ws2_32 wsock32)
endif()
if(APPLE)
    target_link_libraries(lemonade-agent-host PRIVATE "-framework Foundation" "-framework CoreServices")
endif()
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(lemonade-agent-host PRIVATE Threads::Threads)
endif()
