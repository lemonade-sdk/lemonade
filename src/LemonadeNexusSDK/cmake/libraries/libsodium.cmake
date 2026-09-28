find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT APPLE AND NOT WIN32)
    pkg_check_modules(NEXUS_SODIUM QUIET IMPORTED_TARGET libsodium>=1.0.18)
    if(TARGET PkgConfig::NEXUS_SODIUM)
        add_library(sodium INTERFACE)
        target_link_libraries(sodium INTERFACE PkgConfig::NEXUS_SODIUM)
        return()
    endif()
endif()

cmake_minimum_required(VERSION 3.20)

include(FetchContent)

FetchContent_Declare(Sodium
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG efe978b574516d7708c0feeb5231cf7ab4906283  # libsodium 1.0.20
)
set(SODIUM_DISABLE_TESTS ON)
FetchContent_MakeAvailable(Sodium)

# Endianness detection — libsodium needs NATIVE_LITTLE_ENDIAN for ARM crypto
include(TestBigEndian)
test_big_endian(_SODIUM_BIG_ENDIAN)
if(NOT _SODIUM_BIG_ENDIAN)
    target_compile_definitions(sodium PRIVATE NATIVE_LITTLE_ENDIAN=1)
endif()

# x86_64 AES-256-GCM: the robinlinden wrapper only sets -maes/-mpclmul/-mssse3
# inside a clang-cl block (Windows). On Linux/macOS with GCC or Clang, the
# AES-NI intrinsics are never enabled, causing crypto_aead_aes256gcm_is_available()
# to return 0 even on CPUs with AES-NI support (e.g. Xeon v4).
# HAVE_TMMINTRIN_H + HAVE_WMMINTRIN_H are required by libsodium's guard macro
# to compile the actual AES-NI implementation instead of the ENOSYS stub.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(sodium PRIVATE -maes -mpclmul -mssse3 -mavx)
    target_compile_definitions(sodium PRIVATE
        HAVE_TMMINTRIN_H=1
        HAVE_WMMINTRIN_H=1
    )
endif()

# ARM64 AES-256-GCM: the robinlinden wrapper only includes the AES-NI (x86)
# variant.  Add the ARM crypto implementation so Apple Silicon / aarch64 Linux
# get hardware-accelerated AES-256-GCM instead of the stub that returns 0.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    target_sources(sodium PRIVATE
        ${sodium_SOURCE_DIR}/libsodium/src/libsodium/crypto_aead/aes256gcm/armcrypto/aead_aes256gcm_armcrypto.c
    )
    target_compile_definitions(sodium PRIVATE HAVE_ARMCRYPTO=1)
    target_compile_options(sodium PRIVATE -march=armv8-a+crypto)
endif()
