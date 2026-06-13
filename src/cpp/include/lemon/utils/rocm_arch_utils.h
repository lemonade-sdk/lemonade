#pragma once

#include <string>

namespace lemon {

std::string rocm_arch_numeric_to_gfx(const std::string& numeric_version);
bool rocm_arch_is_valid_gfx(const std::string& gfx_arch);


}  // namespace lemon