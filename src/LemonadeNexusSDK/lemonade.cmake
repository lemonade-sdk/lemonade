if(NOT LEMONADE_WITH_NEXUS)
    return()
endif()
include(ExternalProject)
set(LEMONADE_NEXUS_CMAKE_ARGS "" CACHE STRING "Additional client SDK CMake arguments")
set(_nexus_deployment_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(APPLE AND NOT _nexus_deployment_target)
    set(_nexus_deployment_target "10.15")
endif()
set(_nexus_build "${CMAKE_BINARY_DIR}/nexus-sdk")
set(_nexus_link_dir "${_nexus_build}")
set(_nexus_runtime "${_nexus_build}/${CMAKE_SHARED_LIBRARY_PREFIX}lemonade_nexus_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}")
if(CMAKE_CONFIGURATION_TYPES)
    set(_nexus_link_dir "${_nexus_build}/Release")
    set(_nexus_runtime "${_nexus_build}/Release/${CMAKE_SHARED_LIBRARY_PREFIX}lemonade_nexus_sdk${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
set(_nexus_byproducts "${_nexus_runtime}")
if(WIN32)
    list(APPEND _nexus_byproducts "${_nexus_link_dir}/lemonade_nexus_sdk.lib")
endif()
ExternalProject_Add(nexus-sdk-build
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/LemonadeNexusSDK"
    BINARY_DIR "${_nexus_build}"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DOPENSSL_FORCE_BUNDLED=$<BOOL:${APPLE}>
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${_nexus_deployment_target}
        ${LEMONADE_NEXUS_CMAKE_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target lemonade_nexus_sdk
    INSTALL_COMMAND ""
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS ${_nexus_byproducts}
)
add_library(lemonade-nexus-sdk SHARED IMPORTED GLOBAL)
set_target_properties(lemonade-nexus-sdk PROPERTIES IMPORTED_LOCATION "${_nexus_runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/LemonadeNexusSDK/include")
if(WIN32)
    set_target_properties(lemonade-nexus-sdk PROPERTIES IMPORTED_IMPLIB "${_nexus_link_dir}/lemonade_nexus_sdk.lib")
endif()
add_dependencies(lemonade-nexus-sdk nexus-sdk-build)
target_compile_definitions(lemonade-server-core PUBLIC LEMONADE_WITH_NEXUS=1)
target_link_libraries(lemonade-server-core PUBLIC lemonade-nexus-sdk)
foreach(_server lemond LemonadeServer)
    if(TARGET ${_server})
        add_custom_command(TARGET ${_server} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_nexus_runtime}" "$<TARGET_FILE_DIR:${_server}>/"
            VERBATIM)
    endif()
endforeach()
if(APPLE)
    foreach(_server lemond LemonadeServer)
        if(TARGET ${_server})
            set_property(TARGET ${_server} APPEND PROPERTY INSTALL_RPATH "@loader_path")
        endif()
    endforeach()
elseif(UNIX)
    set_property(TARGET lemond APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../lib/lemonade")
endif()
if(WIN32 OR APPLE)
    install(PROGRAMS "${_nexus_runtime}" DESTINATION bin COMPONENT Runtime)
else()
    install(FILES "${_nexus_runtime}" DESTINATION lib/lemonade COMPONENT Runtime)
endif()
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    install(CODE "
        execute_process(COMMAND codesign --force --options runtime --timestamp
            --sign \"${SIGNING_IDENTITY}\" \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/liblemonade_nexus_sdk.dylib\"
            RESULT_VARIABLE nexus_sign_result)
        if(NOT nexus_sign_result EQUAL 0)
            message(FATAL_ERROR \"Failed to sign Nexus client SDK\")
        endif()
    " COMPONENT Runtime)
endif()
