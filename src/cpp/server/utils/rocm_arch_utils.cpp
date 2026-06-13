#include <iostream>
#include <string>
#include <regex>

namespace lemon{


bool rocm_arch_is_valid_gfx(const std::string& gfx_arch) {
   std::smatch gfx_match;
   return std::regex_search(gfx_arch, gfx_match, std::regex(R"((gfx\d{4}))"));
}

// In this function we tranform the rocm archictecture from numeric format to gfx format.
std::string rocm_arch_numeric_to_gfx(const std::string& numeric_arch) {
    try {
        // We convert the string with numeric version to long long number
        long long num = std::stoll(numeric_arch);
        
        // We get the differents components of version.
        long long major = num / 10000;
        long long minor = (num / 100) % 100;
        long long stepping = num % 100;
        
        // We build the gfx version with the previous components.
        return "gfx" + std::to_string(major) + std::to_string(minor) + std::to_string(stepping);
    } 
    catch (const std::invalid_argument& e) {
        // The case that the conversion of stoll cannot be done.
        throw std::invalid_argument("The numeric version is not a valid number");
    } 
    catch (const std::out_of_range& e) {
        // The case that the conversion is bigger than long long limits.
        throw std::invalid_argument("The numeric version is not a valid number");
    }
}

} // namespace lemon