// SPDX-License-Identifier: MIT
// Copyright (C) 2024-2025, Advanced Micro Devices, Inc. All rights reserved.
#ifndef AIEBU_ERROR_H_
#define AIEBU_ERROR_H_
#include "aiebu/aiebu.h"

#include <string>
#include <system_error>

namespace aiebu {

class error : public std::system_error
{
public:
  enum class error_code : int {
    invalid_asm = aiebu_invalid_asm,
    invalid_patch_schema = aiebu_invalid_patch_schema,
    invalid_patch_buffer_type = aiebu_invalid_batch_buffer_type,
    invalid_buffer_type = aiebu_invalid_buffer_type,
    invalid_offset = aiebu_invalid_offset,
    internal_error = aiebu_invalid_internal_error,
    invalid_input = aiebu_invalid_input,
    invalid_elf = aiebu_invalid_elf,
    invalid_opcode = aiebu_invalid_opcode
  };

  error(error_code ec, const std::error_category& cat, const std::string& what = "");

  explicit
  error(error_code ec, const std::string& what = "");

  // Retrive underlying code for return plain error code
  int
  value() const;

  int
  get() const;

  int
  get_code() const;
};

}

#endif // AIEBU_ERROR_H_
