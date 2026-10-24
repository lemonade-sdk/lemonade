# =============================================================================
# Custom FindOpenSSL.cmake — short-circuit when targets already exist
# =============================================================================
# This file lives in ${PROJECT_SOURCE_DIR}/cmake/ which is on CMAKE_MODULE_PATH,
# so it takes precedence over CMake's built-in FindOpenSSL.cmake.
#
# When cmake/libraries/openssl.cmake has already provided OpenSSL (either from
# system find_package or source build), the targets exist and we just report
# success.  This prevents httplib, jwt-cpp, and other FetchContent deps from
# re-running the built-in FindOpenSSL and potentially picking up a different one.
# =============================================================================

if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    # Targets already exist — set the standard result variables so
    # find_package_handle_standard_args and downstream checks are happy.
    set(OpenSSL_FOUND TRUE)
    set(OPENSSL_FOUND TRUE)

    # Component handling — mark all requested components as found.
    foreach(_comp IN LISTS OpenSSL_FIND_COMPONENTS)
        set(OpenSSL_${_comp}_FOUND TRUE)
    endforeach()

    # Populate aggregate variables that some projects check directly.
    if(OPENSSL_SSL_LIBRARY AND OPENSSL_CRYPTO_LIBRARY)
        set(OPENSSL_LIBRARIES    "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}")
        set(OPENSSL_SSL_LIBRARIES    "${OPENSSL_SSL_LIBRARY}")
        set(OPENSSL_CRYPTO_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}")
    endif()

    if(NOT OpenSSL_FIND_QUIETLY)
        message(STATUS "FindOpenSSL: using existing OpenSSL targets")
    endif()
    return()
endif()

# If we get here, openssl.cmake hasn't provided targets yet (shouldn't happen
# in normal builds).  Fall through to CMake's built-in FindOpenSSL.
include(${CMAKE_ROOT}/Modules/FindOpenSSL.cmake)
