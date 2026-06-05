/// \file typedef.hpp
/// \brief typedef file for the FastFlowLM project
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This file contains the typedefs for the FastFlowLM project

#pragma once
#include <cstdint>
#include <iostream>
#include "buffer.hpp"
#include <immintrin.h>
#include <math.h>
#include <codecvt>
#include <locale>
#include "biovault_bfloat16.h"

typedef float f32;
typedef std::int8_t i8;
typedef std::int16_t i16;
typedef std::int32_t i32;
typedef std::int64_t i64;
typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;

/// \brief accdtype is commonly used in LLM models
typedef f32 accdtype;

typedef enum: u8 {cpu, npu} device_t;

/// \brief JSON GET macro
/// \param output the output variable
/// \param json_handler the json handler
/// \param key the key to get the value from
/// \param default_value the default value to assign if the key does not exist
/// \param data_type the data type of the output
/// \note if the key exists, assign the value to the output
/// \note otherwise, assign the default value to the output
/// \note Usage: JSON_GET(output, json_handler, key, default_value, data_type)
/// \note   output: the output variable
//   json_handler: the json handler
//   key: the key to get the value from
//   default_value: the default value to assign if the key does not exist
//   data_type: the data type of the output
// Example: JSON_GET(this->model_desc.vocab_size, json_handler["config"], "vocab_size", 0, i32);
#define JSON_GET(output, json_handler, key, default_value, data_type) if (json_handler.contains(key) && !json_handler[key].is_null()) { \
    output = data_type(json_handler[key]); \
} else { \
    output = default_value; \
}

typedef biovault::bfloat16_t bf16;
typedef biovault::bfloat16_t bf16_t;
typedef bf16 dtype;
typedef buffer<bf16> vdtype;

/// \brief Helper functions for bf16 array reinterpretation
/// \param u16_arr u16 array
/// \param size size of the array
/// \return bf16 array view (does not own memory)
inline bf16* reinterpret_u16_as_bf16(u16* u16_arr, size_t size) {
    return reinterpret_cast<bf16*>(u16_arr);
}

/// \brief Helper functions for bf16 array reinterpretation (const version)
/// \param u16_arr const u16 array
/// \param size size of the array
/// \return const bf16 array view (does not own memory)
inline const bf16* reinterpret_u16_as_bf16(const u16* u16_arr, size_t size) {
    return reinterpret_cast<const bf16*>(u16_arr);
}

/// \brief Helper functions for bf16 array reinterpretation
/// \param bf16_arr bf16 array
/// \param size size of the array
/// \return u16 array view (does not own memory)
inline u16* reinterpret_bf16_as_u16(bf16* bf16_arr, size_t size) {
    return reinterpret_cast<u16*>(bf16_arr);
}

/// \brief Helper functions for bf16 array reinterpretation (const version)
/// \param bf16_arr const bf16 array
/// \param size size of the array
/// \return const u16 array view (does not own memory)
inline const u16* reinterpret_bf16_as_u16(const bf16* bf16_arr, size_t size) {
    return reinterpret_cast<const u16*>(bf16_arr);
}

/// \brief AVX2 Convert bfloat16 to float32
/// \param bf16_vals the bfloat16 values, 128-bits, 8xbf16
/// \return the float32 values, 256-bits, 8xfloat32
inline __m256 bf16o_fp32(__m128i bf16_vals) {
    __m256i expanded = _mm256_cvtepu16_epi32(bf16_vals); // Extend to 32-bit
    return _mm256_castsi256_ps(_mm256_slli_epi32(expanded, 16));  // Shift to float position
}

/// \brief AVX2 Convert float32 to bfloat16
/// \param fp32_vals the float32 values, 256-bits, 8xfloat32
/// \return the bfloat16 values, 128-bits, 8xbf16
inline __m128i f32o_bf16(__m256 fp32_vals) {
    __m256i rounded = _mm256_srli_epi32(_mm256_castps_si256(fp32_vals), 16);  // Truncate lower bits
    return _mm_packus_epi32(_mm256_extracti128_si256(rounded, 0),
                            _mm256_extracti128_si256(rounded, 1));
}

/// \brief AVX-512 Convert bfloat16 to float32 (16 lanes)
/// \param bf16_vals the bfloat16 values, 256-bits, 16xbf16
/// \return the float32 values, 512-bits, 16xfloat32
inline __m512 bf16o_fp32_512(__m256i bf16_vals) {
    __m512i expanded = _mm512_cvtepu16_epi32(bf16_vals);
    return _mm512_castsi512_ps(_mm512_slli_epi32(expanded, 16));
}

/// \brief AVX-512 Convert float32 to bfloat16 (16 lanes)
/// \param fp32_vals the float32 values, 512-bits, 16xfloat32
/// \return the bfloat16 values, 256-bits, 16xbf16
// inline __m256i f32o_bf16_512(__m512 fp32_vals) {
//     __m512i shifted = _mm512_srli_epi32(_mm512_castps_si512(fp32_vals), 16);  // Truncate lower bits
//     return _mm512_cvtepi32_epi16(shifted);
// }
inline __m256i f32o_bf16_512(__m512 fp32_vals) {
    __m256 lower = _mm512_castps512_ps256(fp32_vals);
    __m256 upper = _mm512_extractf32x8_ps(fp32_vals, 1);

    __m128i lower_bf16 = f32o_bf16(lower);
    __m128i upper_bf16 = f32o_bf16(upper);

    __m256i result = _mm256_castsi128_si256(lower_bf16);
    result = _mm256_inserti128_si256(result, upper_bf16, 1);
    return result;
}

/// \brief Convert UTF-8 string to wide string
/// \param str the UTF-8 string
/// \return the wide string
/// \note Usage: std::wstring wstr = utf8_to_wstring(str);
#ifdef __WINDOWS__
inline std::wstring utf8_to_wstring(const std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size_needed);
    return wstr;
}
#else
inline std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str);
}
#endif
