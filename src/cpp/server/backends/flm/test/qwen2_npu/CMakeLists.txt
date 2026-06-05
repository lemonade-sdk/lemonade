cmake_minimum_required(VERSION 3.22)
project(qwen2_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_qwen2_npu
    test/qwen2_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_qwen2.cpp"
)

target_link_libraries(test_qwen2_npu PUBLIC
    qwen2_npu
    xrt_coreutil
)

# Add test target
add_custom_target(test_qwen2_npu_target
    DEPENDS test_qwen2_npu
    COMMENT "Building test_qwen2_npu executable"
)
