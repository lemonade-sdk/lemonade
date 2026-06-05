cmake_minimum_required(VERSION 3.22)
project(lfm2_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_lfm2_npu
    test/lfm2_npu
    USE_AUTOMODEL
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/AutoModel/modeling_lfm2.cpp"
)

# Configure runtime linking to support both static Boost and dynamic tokenizers_cpp
if(MSVC)
    # Use dynamic runtime to match tokenizers_cpp library
    set_property(TARGET test_lfm2_npu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_property(TARGET test_lfm2_npu APPEND_STRING PROPERTY LINK_FLAGS " /NODEFAULTLIB:LIBCMT")
endif()

target_link_directories(test_lfm2_npu PUBLIC
    ${CMAKE_SOURCE_DIR}/../../external_libs
)

target_link_libraries(test_lfm2_npu PUBLIC
    lfm2_npu
    xrt_coreutil
    aiebu_static
    libboost_program_options-vc143-mt-x64-1_88
)

# Add test target
add_custom_target(test_lfm2_npu_target
    DEPENDS test_lfm2_npu
    COMMENT "Building test_lfm2_npu executable"
)
