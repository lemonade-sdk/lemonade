#ifndef BIOVAULT_BFLOAT16_H_INCLUDE_GUARD
#define BIOVAULT_BFLOAT16_H_INCLUDE_GUARD

/*******************************************************************************
* Copyright 2020 LKEB, Leiden University Medical Center
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

// Adapted from the original dnnl::impl::bfloat16_t implementation of
// oneAPI Deep Neural Network Library (oneDNN), by Intel Corporation,
// which is licensed under the Apache License, Version 2.0:
// https://github.com/oneapi-src/oneDNN/blob/v1.7/LICENSE

//NOTE: direct copy from https://github.com/biovault/biovault_bfloat16/blob/main/biovault_bfloat16.h
// commit: 5cdca6a3f14a575aef55278241deb47697edca76


#include <array>
#include <cstdint> // For uint16_t and uint32_t.
#include <cmath>
#include <cfloat>
#include <cstring>
#include <type_traits> // For enable_if, is_integral, is_trivially_copyable, and is_pod.


// For a tagged version of the biovault_bfloat16 repository, having tag name
// "v" <major> "." <minor> "." <patch>, the following macro defines should
// match that tag:
#define BIOVAULT_BFLOAT16_MAJOR_VERSION 1
#define BIOVAULT_BFLOAT16_MINOR_VERSION 0
#define BIOVAULT_BFLOAT16_PATCH_VERSION 2


#ifdef _MSC_VER
#	if _MSC_VER < 1900
// Before Visual Studio 2015, Visual C++ did not yet support constexpr
#	define BIOVAULT_BFLOAT16_CONSTEXPR
#	endif
#endif

#ifndef BIOVAULT_BFLOAT16_CONSTEXPR
#define BIOVAULT_BFLOAT16_CONSTEXPR constexpr
#endif

namespace biovault {

	class bfloat16_t {

	private:
		// Ensure that the following integer types can be used without "std::" prefix,
		// just like in the original implementation (dnnl::impl::bfloat16_t).
		using uint16_t = std::uint16_t;
		using uint32_t = std::uint32_t;

		uint16_t raw_bits_;

		// bit_cast implementation originally from oneDNN:
		// https://github.com/oneapi-src/oneDNN/blob/v1.7/src/common/bit_cast.hpp
		//
		// Returns a value of type T by reinterpretting the representation of the input
		// value (part of C++20).
		//
		// Provides a safe implementation of type punning.
		//
		// Constraints:
		// - U and T must have the same size
		// - U and T must be trivially copyable
		template <typename T, typename U>
		static T bit_cast(const U& u) {
			static_assert(sizeof(T) == sizeof(U), "Bit-casting must preserve size.");
#if __cplusplus >= 202002L
    			// C++20 (and later) code
			static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable.");
			static_assert(std::is_trivially_copyable<U>::value, "U must be trivially copyable.");
#else
			// Use std::is_pod as older GNU versions do not support
			// std::is_trivially_copyable.
			static_assert(std::is_pod<T>::value, "T must be trivially copyable.");
			static_assert(std::is_pod<U>::value, "U must be trivially copyable.");
#endif

			T t;
			std::memcpy(&t, &u, sizeof(U));
			return t;
		}

		// Converts the 32 bits of a normal float or zero to the bits of a bfloat16.
		static BIOVAULT_BFLOAT16_CONSTEXPR uint16_t convert_bits_of_normal_or_zero(
			const uint32_t bits) {
			return uint32_t{
							bits + uint32_t {0x7FFFU + (uint32_t {bits >> 16} &1U)} }
			>> 16;
		}


	public:
		bfloat16_t() = default;

		// Allows specifying a bfloat16 by its raw bits. Equivalent to C++20
		// std::bit_cast<bfloat16_t>(r) (which is more generic, of course.)
		// Originally from oneDNN:
		// https://github.com/oneapi-src/oneDNN/blob/v1.7/src/common/bfloat16.hpp#L34
		BIOVAULT_BFLOAT16_CONSTEXPR bfloat16_t(const uint16_t r, bool) : raw_bits_(r) {}

		// Supports narrowing (lossy) conversion from 32-bit float to bfloat16.
		// Note: This constructor is "explicit" by default, but can be adjusted
		// to allow implicit conversion to bfloat16_t by defining the macro
		// BIOVAULT_BFLOAT16_CONVERTING_CONSTRUCTORS. (The oneDNN library does allow
		// implicit conversion from float to bfloat_t.)
#ifndef BIOVAULT_BFLOAT16_CONVERTING_CONSTRUCTORS
		explicit
#endif
			bfloat16_t(const float f) {
			// Implementation originally from oneDNN:
			// https://github.com/oneapi-src/oneDNN/blob/v1.7/src/cpu/bfloat16.cpp#L47-L69
			auto iraw = bit_cast<std::array<uint16_t, 2>>(f);
			switch (std::fpclassify(f)) {
			case FP_SUBNORMAL:
			case FP_ZERO:
				// sign preserving zero (denormal go to zero)
				raw_bits_ = iraw[1];
				raw_bits_ &= 0x8000;
				break;
			case FP_INFINITE: raw_bits_ = iraw[1]; break;
			case FP_NAN:
				// truncate and set MSB of the mantissa force QNAN
				raw_bits_ = iraw[1];
				raw_bits_ |= 1 << 6;
				break;
			case FP_NORMAL:
				// round to nearest even and truncate
				const uint32_t rounding_bias = 0x00007FFF + (iraw[1] & 0x1);
				const uint32_t int_raw
					= bit_cast<uint32_t>(f) + rounding_bias;
				iraw = bit_cast<std::array<uint16_t, 2>>(int_raw);
				raw_bits_ = iraw[1];
				break;
			}
		}


		// Supports possibly narrowing (lossy) conversion from any integer type.
		// Equivalent to bfloat16_t{static_cast<float>(i)}, but significantly faster.
		// Note: This constructor is "explicit" by default, but can be adjusted
		// to allow implicit conversion to bfloat16_t by defining the macro
		// BIOVAULT_BFLOAT16_CONVERTING_CONSTRUCTORS.
		template <typename IntegerType,
			typename SFINAE = typename std::enable_if<
			std::is_integral<IntegerType>::value>::type>
#ifndef BIOVAULT_BFLOAT16_CONVERTING_CONSTRUCTORS
			explicit
#endif
			bfloat16_t(const IntegerType i)
			: raw_bits_{ convert_bits_of_normal_or_zero(
				bit_cast<uint32_t>(static_cast<float>(i))) }
		{
		}

		bfloat16_t& operator=(const float f) {
			return (*this) = bfloat16_t{ f };
		}

		template <typename IntegerType,
			typename SFINAE = typename std::enable_if<
			std::is_integral<IntegerType>::value>::type>
			bfloat16_t& operator=(const IntegerType i) {
			// Call the converting constructor that is optimized for integer types,
			// followed by the fast defaulted move-assignment operator.
			return (*this) = bfloat16_t{ i };
		}

		// NOLINTNEXTLINE Allow implicit conversion to float, because it is lossless.
		operator float() const {
			// Implementation originally from:
			// https://github.com/oneapi-src/oneDNN/blob/v1.7/src/cpu/bfloat16.cpp#L75-L76
			std::array<uint16_t, 2> iraw = { {0, raw_bits_} };
			return bit_cast<float>(iraw);
		}

		bfloat16_t& operator+=(const float a) {
			(*this) = bfloat16_t{ float{*this} + a };
			return *this;
		}

		friend BIOVAULT_BFLOAT16_CONSTEXPR uint16_t get_raw_bits(const bfloat16_t&);
	};

	// Allows retrieving the raw bits of a bfloat16. Equivalent to C++20
	// std::bit_cast<uint16_t>(bf16) (which is more generic, of course.)
	inline BIOVAULT_BFLOAT16_CONSTEXPR std::uint16_t get_raw_bits(const bfloat16_t& bf16)
	{
		return bf16.raw_bits_;
	}

	static_assert(sizeof(bfloat16_t) == 2, "bfloat16_t must be 2 bytes");

}

#endif
