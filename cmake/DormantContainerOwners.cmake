include(${CMAKE_CURRENT_LIST_DIR}/CxxStringLiterals.cmake)

file(GLOB _dormant_owner_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/server/backends/configured/dormant_container_owners/*.json")
set(_dormant_owner_code "")
foreach(_file IN LISTS _dormant_owner_files)
    file(READ "${_file}" _document)
    string(JSON _id GET "${_document}" id)
    string(JSON _digest GET "${_document}" digest)
    string(JSON _semantics GET "${_document}" semantics)
    if(_semantics MATCHES "\\)owner\\\"")
        message(FATAL_ERROR "${_file}: reserved raw string delimiter")
    endif()
    lemon_cxx_raw_string_literals(_literal owner "${_file}" "${_semantics}")
    string(APPEND _dormant_owner_code
        "    if (id == \"${_id}\") {\n        static const BuiltinContainerExecutionOwner owner{{\"${_id}\", 1}, nlohmann::json::parse(${_literal}), \"${_digest}\"};\n        return owner;\n    }\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/include/dormant_container_owners.h"
    "#pragma once\n${_dormant_owner_code}")
