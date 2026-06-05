cmake_minimum_required(VERSION 3.22)
project(qwen3_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_qwen3_npu
    test/qwen3_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_qwen3.cpp"
)

target_link_libraries(test_qwen3_npu PUBLIC
    qwen3_npu
    xrt_coreutil
    avformat
    avcodec
    avutil
    swscale
    zlib
)

# Add test target
add_custom_target(test_qwen3_npu_target
    DEPENDS test_qwen3_npu
    COMMENT "Building test_qwen3_npu executable"
)
