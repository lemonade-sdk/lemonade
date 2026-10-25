cmake_minimum_required(VERSION 3.25.1)
project(LemonadeNexusClientSDK LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Builds the vendored Nexus client SDK (src/LemonadeNexusSDK, see UPSTREAM.json) as one
# shared library. Lemonade's build rules live here so the vendored tree stays upstream.
set(NEXUS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../src/LemonadeNexusSDK" CACHE PATH "Vendored Nexus SDK")
get_filename_component(NEXUS_SDK_ROOT "${NEXUS_SDK_ROOT}" ABSOLUTE)
set(NEXUS_SDK_PROJECT "${NEXUS_SDK_ROOT}/projects/LemonadeNexusSDK")
list(APPEND CMAKE_MODULE_PATH "${NEXUS_SDK_ROOT}/cmake")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
include("${NEXUS_SDK_ROOT}/cmake/libraries/openssl.cmake")
include(libraries/nlohmann_json.cmake)
include(libraries/spdlog.cmake)
include(libraries/httplib.cmake)
include(libraries/libsodium.cmake)
include(RustTransport.cmake)
file(GLOB_RECURSE _sdk_sources CONFIGURE_DEPENDS "${NEXUS_SDK_PROJECT}/src/*.cpp")
# The SDK's Tier 1 verification compiles these server sources, as upstream's SDK build does.
set(_nexus_server_sources
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Api/TreeRouteGate.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Crypto/CryptoTypes.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Crypto/SecureBuffer.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/CanonicalEncoding.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Consensus/CommitProof.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Consensus/ConsensusTypes.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Consensus/QuorumValidation.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Consensus/VoteKey.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Epoch/AuthorityChain.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Epoch/NextEpochPlan.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Epoch/Tier1Membership.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Epoch/Tier1Set.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Genesis/BootstrapCertificate.cpp"
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Security/Transport/AuthorityChainCodec.cpp"
)
add_library(lemonade_nexus_sdk SHARED ${_sdk_sources} ${_nexus_server_sources}
    "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/src/Boringtun/UserspaceDataplane.cpp")
target_compile_features(lemonade_nexus_sdk PRIVATE cxx_std_20)
target_include_directories(lemonade_nexus_sdk PUBLIC "${NEXUS_SDK_PROJECT}/include"
    PRIVATE "${NEXUS_SDK_ROOT}/projects/LemonadeNexus/include")
target_link_libraries(lemonade_nexus_sdk PRIVATE nlohmann_json::nlohmann_json
    httplib::httplib spdlog sodium OpenSSL::SSL OpenSSL::Crypto
    boringtun-ffi virtual-netstack)
set_target_properties(lemonade_nexus_sdk PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON
    MACOSX_RPATH ON INSTALL_NAME_DIR "@rpath")
if(WIN32)
    target_compile_definitions(lemonade_nexus_sdk PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(lemonade_nexus_sdk PRIVATE ws2_32 mswsock iphlpapi)
endif()
if(UNIX AND NOT APPLE)
    target_link_options(lemonade_nexus_sdk PRIVATE "LINKER:--exclude-libs,ALL")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${NEXUS_SDK_PROJECT}/include/LemonadeNexusSDK/lemonade_nexus.h")
file(STRINGS "${NEXUS_SDK_PROJECT}/include/LemonadeNexusSDK/lemonade_nexus.h" _c_api_lines REGEX "ln_[a-z0-9_]+\\(")
set(_exports "")
foreach(_line IN LISTS _c_api_lines)
    string(REGEX MATCH "(ln_[a-z0-9_]+)\\(" _match "${_line}")
    if(CMAKE_MATCH_1)
        list(APPEND _exports "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _exports)
if(APPLE)
    set(_export_file "${CMAKE_CURRENT_BINARY_DIR}/sdk-exports.txt")
    file(WRITE "${_export_file}" "")
    foreach(_symbol IN LISTS _exports)
        file(APPEND "${_export_file}" "_${_symbol}\n")
    endforeach()
    target_link_options(lemonade_nexus_sdk PRIVATE "LINKER:-exported_symbols_list,${_export_file}")
elseif(WIN32)
    set(_export_file "${CMAKE_CURRENT_BINARY_DIR}/sdk-exports.def")
    file(WRITE "${_export_file}" "EXPORTS\n")
    foreach(_symbol IN LISTS _exports)
        file(APPEND "${_export_file}" "${_symbol}\n")
    endforeach()
    target_sources(lemonade_nexus_sdk PRIVATE "${_export_file}")
    set_target_properties(lemonade_nexus_sdk PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()

if(DEFINED _export_file)
    set_property(TARGET lemonade_nexus_sdk APPEND PROPERTY LINK_DEPENDS "${_export_file}")
endif()
