cmake_minimum_required(VERSION 3.22)
project(gemma_embedding VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_gemma_embedding
    test/gemma_embedding
    USE_TOKENIZER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoEmbeddingModel/auto_embedding_model.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/AutoEmbeddingModel/modeling_gemma_embedding.cpp"
)

target_link_libraries(test_gemma_embedding PUBLIC
    gemma_embedding
    xrt_coreutil
)

# Add test target
add_custom_target(test_gemma_embedding_target
    DEPENDS test_gemma_embedding
    COMMENT "Building test_gemma_embedding executable"
)
