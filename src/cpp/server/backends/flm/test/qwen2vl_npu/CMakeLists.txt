cmake_minimum_required(VERSION 3.22)
project(qwen2vl_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_qwen2vl_npu
    test/qwen2vl_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_qwen2vl.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_qwen2vl_image.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image_process_utils/imageproc.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image_process_utils/imageprocAVX512.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/image/image_reader.cpp"
)

target_link_libraries(test_qwen2vl_npu PUBLIC
    qwen2vl_npu
    xrt_coreutil
    avformat
    avcodec
    avutil
    swscale
    zlib
)

# Add test target
add_custom_target(test_qwen2vl_npu_target
    DEPENDS test_qwen2vl_npu
    COMMENT "Building test_qwen2vl_npu executable"
)
