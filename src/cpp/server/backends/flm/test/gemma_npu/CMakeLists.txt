cmake_minimum_required(VERSION 3.22)
project(gemma_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_gemma_npu
    test/gemma_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_gemma3.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_gemma3_image.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image/image_reader.cpp"
)

target_link_libraries(test_gemma_npu PUBLIC
    gemma_npu
    xrt_coreutil
    avformat
    avcodec
    avutil
    swscale
    zlib
)

# Add test target
add_custom_target(test_gemma_npu_target
    DEPENDS test_gemma_npu
    COMMENT "Building test_gemma_npu executable"
)
