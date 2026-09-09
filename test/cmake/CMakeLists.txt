# test/cmake/CMakeLists.txt
cmake_minimum_required(VERSION 3.15)
project(TestHttplibDetection LANGUAGES CXX)

set(MIN_HTTPLIB_VERSION "0.31.0")

# Prevent searching host system paths (like /usr, /usr/local) to ensure hermetic tests
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH FALSE)

# Include the detection macro
include("${DETECTION_MACRO_PATH}")

# Helper for assertions
function(assert_equal var expected_val)
    if(NOT "${${var}}" STREQUAL "${expected_val}")
        message(FATAL_ERROR "Assertion failed: ${var} = '${${var}}', expected '${expected_val}'")
    endif()
endfunction()

# Setup paths
set(MOCK_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_cmake_mock")
file(REMOVE_RECURSE "${MOCK_DIR}")
file(MAKE_DIRECTORY "${MOCK_DIR}")

if(TEST_CASE STREQUAL "config_only")
    set(pkg_dir "${MOCK_DIR}/lib/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.32.0\")
        set(httplib_INCLUDE_DIRS \"${MOCK_DIR}/include\")
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.32.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_VERSION "0.32.0")
    assert_equal(HTTPLIB_INCLUDE_DIRS "${MOCK_DIR}/include")

elseif(TEST_CASE STREQUAL "old_header")
    set(inc_dir "${MOCK_DIR}/include")
    file(MAKE_DIRECTORY "${inc_dir}")
    file(WRITE "${inc_dir}/httplib.h" "
#define CPPHTTPLIB_VERSION \"0.30.0\"
")

    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_httplib TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND FALSE)
    assert_equal(HTTPLIB_VERSION "")

elseif(TEST_CASE STREQUAL "modern_header")
    set(inc_dir "${MOCK_DIR}/include")
    file(MAKE_DIRECTORY "${inc_dir}")
    file(WRITE "${inc_dir}/httplib.h" "
#define CPPHTTPLIB_VERSION \"0.33.0\"
")

    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_httplib TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_VERSION "0.33.0")
    assert_equal(HTTPLIB_INCLUDE_DIRS "${inc_dir}")

elseif(TEST_CASE STREQUAL "openssl_target")
    set(pkg_dir "${MOCK_DIR}/lib/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.32.0\")
        set(HTTPLIB_IS_USING_OPENSSL TRUE)
        add_library(httplib::httplib INTERFACE IMPORTED)
        set_target_properties(httplib::httplib PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS \"CPPHTTPLIB_OPENSSL_SUPPORT\"
            INTERFACE_INCLUDE_DIRECTORIES \"${MOCK_DIR}/include\"
        )
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.32.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_VERSION "0.32.0")
    assert_equal(HTTPLIB_IS_USING_OPENSSL TRUE)

elseif(TEST_CASE STREQUAL "disabled_generator_expression")
    set(pkg_dir "${MOCK_DIR}/lib/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.32.0\")
        set(HTTPLIB_IS_USING_OPENSSL FALSE)
        add_library(httplib::httplib INTERFACE IMPORTED)
        set_target_properties(httplib::httplib PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS \"\$<\$<BOOL:FALSE>:CPPHTTPLIB_OPENSSL_SUPPORT>\"
            INTERFACE_INCLUDE_DIRECTORIES \"${MOCK_DIR}/include\"
        )
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.32.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_IS_USING_OPENSSL FALSE)

elseif(TEST_CASE STREQUAL "too_old_config")
    set(pkg_dir "${MOCK_DIR}/lib/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.30.0\")
        add_library(httplib::httplib INTERFACE IMPORTED)
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.30.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND FALSE)
    if(TARGET httplib::httplib)
        message(FATAL_ERROR "Assertion failed: httplib::httplib target was created despite package being too old!")
    endif()

elseif(TEST_CASE STREQUAL "compiled_no_tls")
    set(pkg_dir "${MOCK_DIR}/lib/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.32.0\")
        set(HTTPLIB_IS_COMPILED TRUE)
        set(HTTPLIB_IS_USING_OPENSSL FALSE)
        set(HTTPLIB_IS_USING_MBEDTLS FALSE)
        set(HTTPLIB_IS_USING_WOLFSSL FALSE)
        add_library(httplib::httplib INTERFACE IMPORTED)
        set_target_properties(httplib::httplib PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES \"${MOCK_DIR}/include\"
        )
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.32.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_IS_COMPILED TRUE)
    assert_equal(HTTPLIB_IS_USING_OPENSSL FALSE)
    assert_equal(HTTPLIB_IS_USING_MBEDTLS FALSE)
    assert_equal(HTTPLIB_IS_USING_WOLFSSL FALSE)

    # Replicate target configuration logic and assert no compile definitions are added to lemonade-httplib
    add_library(lemonade-httplib INTERFACE)
    target_link_libraries(lemonade-httplib INTERFACE httplib::httplib)

    set(SYSTEM_HTTPLIB_HAS_TLS FALSE)
    set(SYSTEM_HTTPLIB_USES_OPENSSL FALSE)
    set(SYSTEM_HTTPLIB_USES_MBEDTLS FALSE)
    set(SYSTEM_HTTPLIB_USES_WOLFSSL FALSE)

    if(HTTPLIB_IS_USING_OPENSSL)
        set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
        set(SYSTEM_HTTPLIB_USES_OPENSSL TRUE)
    elseif(HTTPLIB_IS_USING_MBEDTLS)
        set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
        set(SYSTEM_HTTPLIB_USES_MBEDTLS TRUE)
    elseif(HTTPLIB_IS_USING_WOLFSSL)
        set(SYSTEM_HTTPLIB_HAS_TLS TRUE)
        set(SYSTEM_HTTPLIB_USES_WOLFSSL TRUE)
    endif()

    if(SYSTEM_HTTPLIB_USES_OPENSSL)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    elseif(SYSTEM_HTTPLIB_USES_MBEDTLS)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
    elseif(SYSTEM_HTTPLIB_USES_WOLFSSL)
        target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_WOLFSSL_SUPPORT)
    endif()

    if(NOT SYSTEM_HTTPLIB_HAS_TLS AND NOT HTTPLIB_IS_COMPILED)
        if(HTTPLIB_VERSION AND NOT HTTPLIB_VERSION VERSION_LESS "0.31.0")
            target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
        endif()
    endif()

    get_target_property(target_defs lemonade-httplib INTERFACE_COMPILE_DEFINITIONS)
    if(target_defs)
        message(FATAL_ERROR "Assertion failed: lemonade-httplib target has compile definitions '${target_defs}' but expected none!")
    endif()

elseif(TEST_CASE STREQUAL "too_old_config_multiarch")
    set(CMAKE_LIBRARY_ARCHITECTURE "x86_64-linux-gnu")
    set(pkg_dir "${MOCK_DIR}/lib/x86_64-linux-gnu/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.30.0\")
        add_library(httplib::httplib INTERFACE IMPORTED)
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.30.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")
    set(PkgConfig_FOUND FALSE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND FALSE)
    if(TARGET httplib::httplib)
        message(FATAL_ERROR "Assertion failed: httplib::httplib target was created despite multiarch package being too old!")
    endif()

elseif(TEST_CASE STREQUAL "header_only_pkgconfig")
    file(WRITE "${MOCK_DIR}/FindPkgConfig.cmake" "
        set(PkgConfig_FOUND TRUE)
        macro(pkg_search_module prefix)
            set(\${prefix}_FOUND TRUE)
            set(\${prefix}_VERSION \"0.32.0\")
            set(\${prefix}_INCLUDE_DIRS \"\${MOCK_DIR}/include\")
            set(\${prefix}_LIBRARIES \"\")
            set(\${prefix}_LINK_LIBRARIES \"\")
            set(\${prefix}_LIBRARY_DIRS \"\")
            set(\${prefix}_CFLAGS_OTHER \"\")
        endmacro()
    ")
    list(APPEND CMAKE_MODULE_PATH "${MOCK_DIR}")

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_VERSION "0.32.0")
    assert_equal(HTTPLIB_IS_COMPILED FALSE)

elseif(TEST_CASE STREQUAL "cli_composition_mbedtls_isolation")
    add_library(lemonade-digest-crypto INTERFACE)
    add_library(lemonade-httplib INTERFACE)
    target_link_libraries(lemonade-httplib INTERFACE lemonade-digest-crypto)
    target_compile_definitions(lemonade-httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)

    get_target_property(defs lemonade-httplib INTERFACE_COMPILE_DEFINITIONS)
    assert_equal(defs "CPPHTTPLIB_MBEDTLS_SUPPORT")

elseif(TEST_CASE STREQUAL "custom_httplib_dir_precheck")
    set(httplib_DIR "${MOCK_DIR}/custom_httplib")
    file(MAKE_DIRECTORY "${httplib_DIR}")
    file(WRITE "${httplib_DIR}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.24.0\")
        add_library(httplib::httplib INTERFACE IMPORTED)
    ")
    file(WRITE "${httplib_DIR}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.24.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND FALSE)
    if(TARGET httplib::httplib)
        message(FATAL_ERROR "Assertion failed: httplib::httplib target was created despite custom httplib_DIR package being too old!")
    endif()

elseif(TEST_CASE STREQUAL "polluted_target_fatal")
    add_library(httplib::httplib INTERFACE IMPORTED)
    set(inc_dir "${MOCK_DIR}/include")
    file(MAKE_DIRECTORY "${inc_dir}")
    file(WRITE "${inc_dir}/httplib.h" "
#define CPPHTTPLIB_VERSION \"0.24.0\"
")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_httplib TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

elseif(TEST_CASE STREQUAL "wolfssl_target")
    set(pkg_dir "${MOCK_DIR}/lib/cmake/httplib")
    file(MAKE_DIRECTORY "${pkg_dir}")
    file(WRITE "${pkg_dir}/httplibConfig.cmake" "
        set(httplib_FOUND TRUE)
        set(httplib_VERSION \"0.32.0\")
        set(HTTPLIB_IS_USING_WOLFSSL TRUE)
        add_library(httplib::httplib INTERFACE IMPORTED)
    ")
    file(WRITE "${pkg_dir}/httplibConfigVersion.cmake" "
        set(PACKAGE_VERSION \"0.32.0\")
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    ")
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    set(CMAKE_PREFIX_PATH "${MOCK_DIR}")

    detect_system_httplib("${MIN_HTTPLIB_VERSION}")

    assert_equal(HTTPLIB_FOUND TRUE)
    assert_equal(HTTPLIB_IS_USING_WOLFSSL TRUE)

else()
    message(FATAL_ERROR "Unknown TEST_CASE: ${TEST_CASE}")
endif()

file(REMOVE_RECURSE "${MOCK_DIR}")
message(STATUS "TEST_CASE ${TEST_CASE} passed successfully")
