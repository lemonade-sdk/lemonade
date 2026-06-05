cmake_minimum_required(VERSION 3.22)
project(flm LANGUAGES CXX VERSION 0.1.0)

if(NOT DEFINED FLM_VERSION)
    message(FATAL_ERROR "FLM_VERSION must be specified externally. Use -DFLM_VERSION=<version> when running cmake.")
endif()

if(NOT DEFINED NPU_VERSION)
    message(FATAL_ERROR "NPU_VERSION must be specified externally. Use -DNPU_VERSION=<version> when running cmake.")
endif()

# Set build type to Release
set(CMAKE_BUILD_TYPE Release)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure CMake to stop at first error
set(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION ON)
set(CMAKE_ERROR_DEPRECATED ON)


# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build/)


# Force output directories to be absolute
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ———————————————————————————————————————————————
# User-tweakable XRT settings (override on cmake command-line)
# ———————————————————————————————————————————————
if(WIN32)
    set(XRT_INCLUDE_DIR C:/dev/XRT/src/runtime_src/core/include CACHE PATH   "Where XRT headers live")
    set(XRT_LIB_DIR     C:/dev/xrtNPUfromDLL     CACHE PATH   "Where XRT libs live")
else()
    set(XRT_INCLUDE_DIR /opt/xilinx/xrt/include CACHE PATH   "Where XRT headers live")
    set(XRT_LIB_DIR     /opt/xilinx/xrt/lib     CACHE PATH   "Where XRT libs live")
endif()

# ———————————————————————————————————————————————
# Add tokenizers-cpp subproject
# ———————————————————————————————————————————————
# Ensure C++17 is available for tokenizers-cpp
if(CMAKE_CXX_STANDARD LESS 17)
    set(CMAKE_CXX_STANDARD 17)
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/../third_party/tokenizers-cpp
                 ${CMAKE_BINARY_DIR}/tokenizers-cpp
                 EXCLUDE_FROM_ALL)

# ———————————————————————————————————————————————
# Gather your sources
# ———————————————————————————————————————————————
file(GLOB SOURCES "src/*.cpp" "runner/*.cpp" "common/*.cpp" "common/*/*.cpp" "server/*.cpp" "pull/*.cpp" )
file(GLOB HEADERS "include/*.hpp" "runner/*.hpp" "common/*.hpp" "common/*/*.hpp" "server/*.hpp" "pull/*.hpp")

# Exclude files that depend on missing libraries for Linux
if(NOT WIN32)
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_gemma3\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_gemma3_image\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_gemma3_text\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_gpt_oss\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_lfm2\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_phi4\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_qwen2\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_qwen3\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_qwen3vl\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_qwen3vl_image\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_whisper\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_whisper_audio\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*modeling_gemma_embedding\\.cpp$")
    # list(FILTER SOURCES EXCLUDE REGEX ".*auto_embedding_model\\.cpp$")

    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/common/image_process_utils/imageprocAVX512.cpp
        ${CMAKE_SOURCE_DIR}/common/audio_process_utils/audioprocAVX512.cpp
        PROPERTIES
        COMPILE_OPTIONS "-mavx512f;-mavx512dq;-mavx512vl;-mavx512bw;-mfma"
    )

    # Define a macro to indicate limited model support on Linux
    # add_compile_definitions(FASTFLOWLM_LINUX_LIMITED_MODELS=1)
endif()


add_executable(flm ${SOURCES} ${HEADERS})

if(NOT WIN32)
    find_package(Threads REQUIRED)
    find_package(Boost REQUIRED COMPONENTS program_options)
    find_package(CURL REQUIRED)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFTW3_PKG fftw3)
        pkg_check_modules(FFTW3F_PKG fftw3f)
        pkg_check_modules(FFTW3L_PKG fftw3l)
        pkg_check_modules(AVFORMAT_PKG libavformat)
        pkg_check_modules(AVCODEC_PKG libavcodec)
        pkg_check_modules(AVUTIL_PKG libavutil)
        pkg_check_modules(SWSCALE_PKG libswscale)
        pkg_check_modules(SWRESAMPLE_PKG libswresample)
    endif()
    find_path(READLINE_INCLUDE_DIR readline/readline.h)
    find_library(READLINE_LIBRARY readline)
    find_library(READLINE_TINFO_LIBRARY tinfo)
    find_library(READLINE_NCURSES_LIBRARY ncurses)
endif()

# ———————————————————————————————————————————————
# Library specific settings
# ———————————————————————————————————————————————
target_include_directories(flm PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/runner
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/pull
    ${XRT_INCLUDE_DIR}
)

if(WIN32)
    target_include_directories(flm PUBLIC
        C:/dev/boost_1_88_0
        C:/dev/vcpkg/installed/x64-windows/include/
    )
endif()

target_compile_definitions(flm PUBLIC
    DISABLE_ABI_CHECK=1
    CMAKE_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
    CMAKE_XCLBIN_PREFIX="${CMAKE_XCLBIN_PREFIX}"
    __FLM_VERSION__=\"${FLM_VERSION}\"
    __NPU_VERSION__=\"${NPU_VERSION}\"
)

if(WIN32)
    target_compile_definitions(flm PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        # Additional definitions for static linking
        CURL_STATICLIB
        BOOST_ALL_NO_LIB
        BOOST_ALL_STATIC_LINK
        # Handle legacy stdio functions
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
        __WINDOWS__
    )
endif()

target_link_directories(flm PUBLIC
    ${XRT_LIB_DIR}
    ${CMAKE_SOURCE_DIR}/lib
)

if(WIN32)
    target_link_directories(flm PUBLIC
        C:/dev/boost_1_88_0/stage/lib
        C:/dev/vcpkg/installed/x64-windows/lib
    )
endif()

# Link static libraries first
if(MSVC)
    target_link_libraries(flm PUBLIC ${STATIC_LIBS})
endif()

# Link your custom libraries (these may still be DLLs if no static versions available)
target_link_libraries(flm PUBLIC
    xrt_coreutil
    q4_npu_eXpress
    llama_npu
    qwen2_npu
    qwen2vl_npu
    qwen3_npu
    qwen3vl_npu
    qwen3_5vl_npu
    gemma_npu
    gemma_text_npu
    gemma4e_npu
    gpt_oss_npu
    whisper_npu
    gemma_embedding
    lfm2_npu
    phi4_npu
    nanbeige_npu
    dequant
    gemm
    lm_head
    mha
)

if(WIN32)
    target_link_libraries(flm PUBLIC
        aiebu_static
    )
else()
    target_link_libraries(flm PUBLIC
        aiebu
    )
endif()

target_link_libraries(flm PUBLIC
    avformat
    avcodec
    avutil
    swscale
    swresample
)

if(WIN32)
    target_link_libraries(flm PUBLIC
        libcurl
        libboost_program_options-vc143-mt-x64-1_88
        libfftw3-3
        libfftw3f-3
        libfftw3l-3
    )
else()
    target_link_libraries(flm PUBLIC
        CURL::libcurl
        Boost::program_options
    )

    target_compile_options(flm PUBLIC -mavx -mavx2)

    if(FFTW3_PKG_FOUND)
        target_include_directories(flm PUBLIC ${FFTW3_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${FFTW3_PKG_LIBRARIES})
    else()
        target_link_libraries(flm PUBLIC fftw3)
    endif()
    if(FFTW3F_PKG_FOUND)
        target_include_directories(flm PUBLIC ${FFTW3F_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${FFTW3F_PKG_LIBRARIES})
    else()
        target_link_libraries(flm PUBLIC fftw3f)
    endif()
    if(FFTW3L_PKG_FOUND)
        target_include_directories(flm PUBLIC ${FFTW3L_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${FFTW3L_PKG_LIBRARIES})
    else()
        target_link_libraries(flm PUBLIC fftw3l)
    endif()

    if(AVFORMAT_PKG_FOUND)
        target_include_directories(flm PUBLIC ${AVFORMAT_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${AVFORMAT_PKG_LIBRARIES})
    endif()
    if(AVCODEC_PKG_FOUND)
        target_include_directories(flm PUBLIC ${AVCODEC_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${AVCODEC_PKG_LIBRARIES})
    endif()
    if(AVUTIL_PKG_FOUND)
        target_include_directories(flm PUBLIC ${AVUTIL_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${AVUTIL_PKG_LIBRARIES})
    endif()
    if(SWSCALE_PKG_FOUND)
        target_include_directories(flm PUBLIC ${SWSCALE_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${SWSCALE_PKG_LIBRARIES})
    endif()
    if(SWRESAMPLE_PKG_FOUND)
        target_include_directories(flm PUBLIC ${SWRESAMPLE_PKG_INCLUDE_DIRS})
        target_link_libraries(flm PUBLIC ${SWRESAMPLE_PKG_LIBRARIES})
    endif()

    if(READLINE_LIBRARY)
        target_compile_definitions(flm PUBLIC FASTFLOWLM_USE_READLINE=1)
        if(READLINE_INCLUDE_DIR)
            target_include_directories(flm PUBLIC ${READLINE_INCLUDE_DIR})
        endif()
        target_link_libraries(flm PUBLIC ${READLINE_LIBRARY})
        if(READLINE_TINFO_LIBRARY)
            target_link_libraries(flm PUBLIC ${READLINE_TINFO_LIBRARY})
        elseif(READLINE_NCURSES_LIBRARY)
            target_link_libraries(flm PUBLIC ${READLINE_NCURSES_LIBRARY})
        endif()
    endif()

    # Link with dl library for dlopen() support (used for preloading bundled libraries)
    target_link_libraries(flm PUBLIC dl)
endif()

# ———————————————————————————————————————————————
# Link tokenizers-cpp libraries
# ———————————————————————————————————————————————
# The tokenizers-cpp subproject provides these targets:
# - tokenizers_cpp: main tokenizers C++ library
# - tokenizers_c: tokenizers C FFI bindings
# - sentencepiece: sentencepiece tokenizer library
target_link_libraries(flm PRIVATE
    tokenizers_cpp
)

if(WIN32)
    target_link_libraries(flm PRIVATE
        ntdll wsock32 ws2_32 Bcrypt
        iphlpapi userenv psapi
    )
else()
    target_link_libraries(flm PRIVATE Threads::Threads)
endif()


# ———————————————————————————————————————————————
# Copy the build flm.exe into the out directory (for local dev)
# ———————————————————————————————————————————————
if(WIN32)
    add_custom_command(TARGET flm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:flm>
            ${CMAKE_SOURCE_DIR}/out/flm.exe
    )
endif()

# Add a custom target to check for remaining DLL dependencies
if(WIN32)
    add_custom_target(check_dependencies ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking for DLL dependencies..."
        COMMAND dumpbin /dependents $<TARGET_FILE:flm> | findstr /i ".dll"
        COMMENT "Checking for remaining DLL dependencies"
        DEPENDS flm
    )
else()
    if(NOT CMAKE_INSTALL_LIBDIR)
        set(CMAKE_INSTALL_LIBDIR "lib")
    endif()

    file(GLOB so_libs "${CMAKE_SOURCE_DIR}/lib/*.so")
    install(FILES ${so_libs} DESTINATION "${CMAKE_INSTALL_LIBDIR}/flm")
    set_target_properties(flm PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/flm")
endif()
install(TARGETS flm
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

install(FILES model_list.json DESTINATION share/flm)

# xclbins, which are loaded by shared libraries need to be in location
# relative to the executable, so we install them to bin/xclbins
install(DIRECTORY xclbins DESTINATION share/flm)

if(NOT WIN32 AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr" AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    install(CODE "
        message(STATUS \"Creating symlink for flm\")
        file(MAKE_DIRECTORY \"$ENV{DESTDIR}/usr/local/bin\")
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
            \"${CMAKE_INSTALL_PREFIX}/bin/flm\"
            \"$ENV{DESTDIR}/usr/local/bin/flm\")"
    )
endif()
