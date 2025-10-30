cmake_minimum_required(VERSION 3.20)
project(ryzenai-serve VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

message(STATUS "Building Ryzen AI LLM Server")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Find dependencies
find_package(Threads REQUIRED)

# ONNX Runtime GenAI
set(OGA_ROOT "C:/Program Files/RyzenAI/1.6.0" CACHE PATH "Path to Ryzen AI installation")

if(NOT EXISTS ${OGA_ROOT})
    message(FATAL_ERROR "Ryzen AI not found at ${OGA_ROOT}. Please install Ryzen AI 1.6.0 or set OGA_ROOT")
endif()

message(STATUS "Using Ryzen AI from: ${OGA_ROOT}")

# Find ONNX Runtime GenAI library (it's in LLM subfolder)
find_library(OGA_LIB 
    NAMES onnxruntime-genai
    PATHS ${OGA_ROOT}/LLM/lib
    REQUIRED
)

find_path(OGA_INCLUDE 
    NAMES ort_genai.h
    PATHS ${OGA_ROOT}/LLM/include
    REQUIRED
)

message(STATUS "ONNX Runtime GenAI library: ${OGA_LIB}")
message(STATUS "ONNX Runtime GenAI include: ${OGA_INCLUDE}")

# Download header-only libraries if not present
include(FetchContent)

# cpp-httplib
set(HTTPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-httplib")
if(NOT EXISTS "${HTTPLIB_DIR}/httplib.h")
    message(STATUS "Downloading cpp-httplib...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.26.0/httplib.h"
        "${HTTPLIB_DIR}/httplib.h"
        SHOW_PROGRESS
    )
endif()

# nlohmann/json
set(JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/json")
if(NOT EXISTS "${JSON_DIR}/json.hpp")
    message(STATUS "Downloading nlohmann/json...")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${JSON_DIR}/json.hpp"
        SHOW_PROGRESS
    )
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/server.cpp
    src/inference_engine.cpp
    src/command_line.cpp
    src/types.cpp
    src/tool_calls.cpp
)

# Create executable
add_executable(ryzenai-serve ${SOURCES})

# Include directories
target_include_directories(ryzenai-serve PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OGA_INCLUDE}
    ${HTTPLIB_DIR}
    ${JSON_DIR}
)

# Link libraries
target_link_libraries(ryzenai-serve PRIVATE
    ${OGA_LIB}
    Threads::Threads
)

# Enable multi-threading for cpp-httplib
target_compile_definitions(ryzenai-serve PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=8)

# Windows-specific settings
if(WIN32)
    # Target Windows 10 or later (required for cpp-httplib v0.26.0)
    target_compile_definitions(ryzenai-serve PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(ryzenai-serve PRIVATE ws2_32)
    
    # Copy required DLLs to output directory (they're in deployment folder)
    set(REQUIRED_DLLS
        "${OGA_ROOT}/deployment/onnxruntime-genai.dll"
        "${OGA_ROOT}/deployment/onnxruntime.dll"
        "${OGA_ROOT}/deployment/onnxruntime_providers_shared.dll"
        "${OGA_ROOT}/deployment/onnxruntime_providers_vitisai.dll"
        "${OGA_ROOT}/deployment/onnxruntime_vitisai_ep.dll"
        "${OGA_ROOT}/deployment/onnxruntime_vitis_ai_custom_ops.dll"
        "${OGA_ROOT}/deployment/onnx_custom_ops.dll"
        "${OGA_ROOT}/deployment/abseil_dll.dll"
        "${OGA_ROOT}/deployment/libutf8_validity.dll"
        "${OGA_ROOT}/deployment/dyn_dispatch_core.dll"
        "${OGA_ROOT}/deployment/DirectML.dll"
        "${OGA_ROOT}/deployment/zlib.dll"
        "${OGA_ROOT}/deployment/zstd.dll"
        "${OGA_ROOT}/deployment/ryzenai_onnx_utils.dll"
        "${OGA_ROOT}/deployment/ryzen_mm.dll"
        "${OGA_ROOT}/deployment/aiecompiler_client.dll"
    )
    
    # Set console subsystem explicitly
    set_target_properties(ryzenai-serve PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE"
    )
    
    foreach(DLL ${REQUIRED_DLLS})
        if(EXISTS ${DLL})
            add_custom_command(TARGET ryzenai-serve POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${DLL}
                $<TARGET_FILE_DIR:ryzenai-serve>
                COMMENT "Copying ${DLL}"
            )
        else()
            message(WARNING "DLL not found: ${DLL}")
        endif()
    endforeach()
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(ryzenai-serve PRIVATE /W4)
else()
    target_compile_options(ryzenai-serve PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Installation rules
install(TARGETS ryzenai-serve
    RUNTIME DESTINATION bin
)

# Print build information
message(STATUS "==========================================================")
message(STATUS "Ryzen AI LLM Server Configuration:")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Ryzen AI path:     ${OGA_ROOT}")
message(STATUS "  Output directory:  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "==========================================================")

