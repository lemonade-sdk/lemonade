# lemonade-agent-host: the per-user process that installs, configures and
# launches agents for lemond. Like the CLI, it compiles the server utilities
# and the CLI's pi sources in place instead of linking lemonade-server-core.

set(LEMON_AGENT_HOST_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_installer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_paths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_pins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/agent_shim.cpp
    PARENT_SCOPE
)
