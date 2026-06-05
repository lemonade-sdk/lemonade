cmake_minimum_required(VERSION 3.22)
project(gemma4e_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

# Find and enable OpenMP for multi-threading
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    message(STATUS "OpenMP found - enabling multi-threading support for test")
else()
    message(WARNING "OpenMP not found - some optimizations will run single-threaded")
endif()

add_npu_test(
    test_gemma4e_npu
    test/gemma4e_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    USE_AVX512
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_gemma4e.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_gemma4e_image.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_gemma4e_audio.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image_process_utils/imageproc.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image_process_utils/imageprocAVX512.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image/image_reader.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/audio/audio_reader.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/audio_process_utils/audioproc.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/audio_process_utils/audioprocAVX512.cpp"
)

target_compile_definitions(test_gemma4e_npu PUBLIC
    USEAVX512=1
    __FLM_VERSION__="0.9.39"
)

# Add OpenMP compiler flags if available
if(OpenMP_CXX_FOUND)
    target_compile_options(test_gemma4e_npu PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/openmp>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fopenmp>
    )
endif()
target_link_directories(test_gemma4e_npu PUBLIC
    ${CMAKE_SOURCE_DIR}/../../external_libs
)

target_link_libraries(test_gemma4e_npu PUBLIC
    gemma4e_npu
    xrt_coreutil
    avformat
    avcodec
    avutil
    swscale
    swresample
    zlib
    libfftw3-3
    libfftw3f-3
    libfftw3l-3
)

# Link OpenMP if available
if(OpenMP_CXX_FOUND)
    target_link_libraries(test_gemma4e_npu PUBLIC OpenMP::OpenMP_CXX)
endif()

# Add test target
add_custom_target(test_gemma4e_npu_target
    DEPENDS test_gemma4e_npu
    COMMENT "Building test_gemma4e_npu executable"
)
