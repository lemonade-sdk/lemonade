cmake_minimum_required(VERSION 3.22)
project(whisper_npu VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
npu_test_setup()

add_npu_test(
    test_whisper_npu
    test/whisper_npu
    USE_TOKENIZER
    USE_SAMPLER
    SOURCES
        "${CMAKE_SOURCE_DIR}/../../common/whisper/modeling_whisper.cpp"
        "${CMAKE_SOURCE_DIR}/../../common/whisper/modeling_whisper_audio.cpp"
)

# Configure runtime linking to support both static Boost and dynamic tokenizers_cpp
if(MSVC)
    # Use dynamic runtime to match tokenizers_cpp library
    set_property(TARGET test_whisper_npu PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_property(TARGET test_whisper_npu APPEND_STRING PROPERTY LINK_FLAGS " /NODEFAULTLIB:LIBCMT")
endif()

target_link_directories(test_whisper_npu PUBLIC
    ${CMAKE_SOURCE_DIR}/../../external_libs
)

target_link_libraries(test_whisper_npu PUBLIC
    whisper_npu
    xrt_coreutil
    aiebu_static
    avformat
    avcodec
    avutil
    swscale
    swresample
    zlib
    libboost_program_options-vc143-mt-x64-1_88
    libfftw3-3
    libfftw3f-3
    libfftw3l-3
)

# Add test target
add_custom_target(test_whisper_npu_target
    DEPENDS test_whisper_npu
    COMMENT "Building test_whisper_npu executable"
)
