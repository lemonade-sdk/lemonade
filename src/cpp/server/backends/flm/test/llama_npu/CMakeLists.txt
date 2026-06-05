cmake_minimum_required(VERSION 3.22)
project(llama_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_llama_npu
    test/llama_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_llama3.cpp"
)

# Configure runtime linking to support both static Boost and dynamic tokenizers_cpp
if(MSVC)
    # Use dynamic runtime to match tokenizers_cpp library
    set_property(TARGET test_llama_npu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_property(TARGET test_llama_npu APPEND_STRING PROPERTY LINK_FLAGS " /NODEFAULTLIB:LIBCMT")
endif()

target_link_libraries(test_llama_npu PUBLIC
    llama_npu
    xrt_coreutil
    aiebu_static
    libboost_program_options-vc143-mt-x64-1_88
)

# Add test target
add_custom_target(test_llama_npu_target
    DEPENDS test_llama_npu
    COMMENT "Building test_llama_npu executable"
)
