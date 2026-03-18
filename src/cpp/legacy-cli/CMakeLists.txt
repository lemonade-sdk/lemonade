# Legacy shim: backwards-compatible lemonade-server binary.
add_executable(lemonade-server main.cpp)

target_include_directories(lemonade-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Link httplib (for stop command's HTTP POST)
if(USE_SYSTEM_HTTPLIB)
    target_link_libraries(lemonade-server PRIVATE cpp-httplib)
else()
    target_link_libraries(lemonade-server PRIVATE httplib::httplib)
endif()
if(USE_SYSTEM_HTTPLIB AND HTTPLIB_INCLUDE_DIRS)
    target_include_directories(lemonade-server PRIVATE ${HTTPLIB_INCLUDE_DIRS})
endif()

# Platform libraries
if(WIN32)
    target_link_libraries(lemonade-server PRIVATE ws2_32)
    target_compile_definitions(lemonade-server PRIVATE UNICODE _UNICODE)
endif()

# Output directory
set_target_properties(lemonade-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    set_target_properties(lemonade-server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

# Install rule
if(NOT APPLE)
    install(TARGETS lemonade-server RUNTIME DESTINATION bin)
endif()

# Create symlink in standard bin path only if not installing to /usr
if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_INSTALL_PREFIX}/bin/lemonade-server
                \"\$ENV{DESTDIR}/usr/bin/lemonade-server\"
        )
    ")
endif()
