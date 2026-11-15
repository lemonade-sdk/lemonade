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
# inside a clang-cl block (Windows). HAVE_TMMINTRIN_H + HAVE_WMMINTRIN_H are
# required by libsodium's guard macro to compile the AES-NI implementation
# instead of the ENOSYS stub.
#
# Limitation: crypto_aead_aes256gcm_is_available() also needs the run-time AVX
# check, and sodium/runtime.c sets has_avx only with HAVE_AVXINTRIN_H and an
# XGETBV path (HAVE__XGETBV or HAVE_AVX_ASM). This file defines neither, so on
# x86_64 the function returns 0 and the AES-256-GCM tests skip. No product code
# calls AES-256-GCM.
#
# libsodium selects the AES-NI code at run time (sodium_runtime_has_aesni,
# _pclmul, _avx). Its own build gives the ISA flags only to the files that hold
# that code (the libaesni group in src/libsodium/Makefile.am). A target-wide
# -mavx lets the compiler emit AVX anywhere in libsodium, which faults with
# SIGILL on a CPU without AVX. Of the libaesni files, only the AES-256-GCM file
# is enabled by the defines below, so only it gets the flags. MSVC does not
# take these flags; clang-cl gets them from the wrapper.
set(_sodium_src ${sodium_SOURCE_DIR}/libsodium/src/libsodium)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_definitions(sodium PRIVATE
        HAVE_TMMINTRIN_H=1
        HAVE_WMMINTRIN_H=1
    )
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(
            ${_sodium_src}/crypto_aead/aes256gcm/aesni/aead_aes256gcm_aesni.c
            TARGET_DIRECTORY sodium
            PROPERTIES COMPILE_OPTIONS "-msse2;-mssse3;-mavx;-maes;-mpclmul"
        )
    endif()
endif()

# ARM64 AES-256-GCM: the robinlinden wrapper only includes the AES-NI (x86)
# variant.  Add the ARM crypto implementation so Apple Silicon / aarch64 Linux
# get hardware-accelerated AES-256-GCM instead of the stub that returns 0.
# libsodium checks for the crypto extension at run time
# (sodium_runtime_has_armcrypto), so only this file gets the crypto flag, as in
# the libarmcrypto group of libsodium's own build.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    target_sources(sodium PRIVATE
        ${_sodium_src}/crypto_aead/aes256gcm/armcrypto/aead_aes256gcm_armcrypto.c
    )
    target_compile_definitions(sodium PRIVATE HAVE_ARMCRYPTO=1)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(
            ${_sodium_src}/crypto_aead/aes256gcm/armcrypto/aead_aes256gcm_armcrypto.c
            TARGET_DIRECTORY sodium
            PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crypto"
        )
    endif()
endif()
unset(_sodium_src)
