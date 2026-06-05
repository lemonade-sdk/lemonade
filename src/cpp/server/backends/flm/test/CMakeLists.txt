# Shared configuration for NPU test targets.

include(CMakeParseArguments)

function(npu_test_setup)
    # Set build type to Release
    set(CMAKE_BUILD_TYPE Release PARENT_SCOPE)

    # Set C++ standard
    set(CMAKE_CXX_STANDARD 20 PARENT_SCOPE)
    set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)

    # Force x64 architecture on Windows
    if(WIN32)
        set(CMAKE_GENERATOR_PLATFORM x64 PARENT_SCOPE)
        set(CMAKE_VS_PLATFORM_NAME "x64" PARENT_SCOPE)
    endif()

    # User-tweakable XRT settings (override on cmake command-line)
    set(XRT_INCLUDE_DIR C:/dev/XRT/src/runtime_src/core/include CACHE PATH "Where XRT headers live")
    set(XRT_LIB_DIR     C:/dev/xrtNPUfromDLL               CACHE PATH "Where XRT libs live")
endfunction()

function(add_npu_test TARGET_NAME OUTPUT_SUBDIR)
    set(options USE_AUTOMODEL USE_TOKENIZER USE_SAMPLER USE_AVX512)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(NPU_TEST "${options}" "" "${multiValueArgs}" ${ARGN})

    set(base_sources "")
    if(NPU_TEST_USE_AUTOMODEL)
        list(APPEND base_sources ${CMAKE_SOURCE_DIR}/../../common/AutoModel/automodel.cpp)
    endif()
    if(NPU_TEST_USE_TOKENIZER)
        list(APPEND base_sources ${CMAKE_SOURCE_DIR}/../../common/tokenizer/tokenizer.cpp)
    endif()
    if(NPU_TEST_USE_SAMPLER)
        list(APPEND base_sources ${CMAKE_SOURCE_DIR}/../../common/modules/sampler.cpp)
    endif()

    list(APPEND base_sources ${CMAKE_SOURCE_DIR}/../../common/utils.cpp)

    # Set test output directory
    set(TEST_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/../../build/${OUTPUT_SUBDIR})
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${TEST_OUTPUT_DIR})

    # Create test executable with basic settings
    add_executable(${TARGET_NAME}
        test.cpp
        ${NPU_TEST_SOURCES}
        ${base_sources}
    )

    # Ensure the standard applies even when using function scope.
    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    # Apply the same AVX2 settings to the test executable
    target_compile_definitions(${TARGET_NAME} PUBLIC
        DISABLE_ABI_CHECK=1
        USEAVX2=1
        _ENABLE_EXTENDED_ALIGNED_STORAGE
        __WINDOWS__
        DEV_BUILD
        CMAKE_INSTALL_PREFIX="${CMAKE_SOURCE_DIR}/../../build/${OUTPUT_SUBDIR}"
        CMAKE_XCLBIN_PREFIX="${CMAKE_SOURCE_DIR}/../../xclbins"
    )

    if(NPU_TEST_USE_AVX512)
        target_compile_options(${TARGET_NAME} PUBLIC
            $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX512 /O2 /fp:fast>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-mavx512f -mavx512dq -mavx512bw -mavx512vl -march=native -O3 -ffast-math>
        )
    else()
        target_compile_options(${TARGET_NAME} PUBLIC
            $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2 /O2 /fp:fast>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-mavx2 -mfma -march=native -O2>
        )
    endif()

    if(MSVC)
        set_target_properties(${TARGET_NAME} PROPERTIES
            LINK_FLAGS "/MACHINE:X64"
        )
    endif()

    # Basic include directories for test
    target_include_directories(${TARGET_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/../../include
        ${XRT_INCLUDE_DIR}
        C:/dev/boost_1_88_0
        C:/dev/vcpkg/installed/x64-windows/include/
    )

    # Basic link settings for test
    target_link_directories(${TARGET_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/../../lib
        ${XRT_LIB_DIR}
        ${CMAKE_SOURCE_DIR}/../../build/tokenizer/lib
        C:/dev/boost_1_88_0/stage/lib
        C:/dev/vcpkg/installed/x64-windows/lib
        # tokenziers_cpp
        ${CMAKE_SOURCE_DIR}/../../build/tokenizers-cpp
        ${CMAKE_SOURCE_DIR}/../../build/tokenizers-cpp/release
        ${CMAKE_SOURCE_DIR}/../../build/tokenizers-cpp/sentencepiece/src/Release
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        q4_npu_eXpress
        lm_head
        dequant
        gemm
        mha
        tokenizers_cpp
        tokenizers_c
        sentencepiece
        ntdll wsock32 ws2_32 Bcrypt
        iphlpapi userenv psapi
    )
endfunction()
