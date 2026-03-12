#pragma once

#include <set>
#include <string>
#include <vector>

namespace lemon {
namespace utils {

inline std::vector<std::string> parse_custom_args(const std::string& custom_args_str) {
    std::vector<std::string> result;
    if (custom_args_str.empty()) {
        return result;
    }

    std::string current_arg;
    bool in_quotes = false;
    char quote_char = '\0';

    for (char c : custom_args_str) {
        if (!in_quotes && (c == '"' || c == '\'')) {
            in_quotes = true;
            quote_char = c;
        } else if (in_quotes && c == quote_char) {
            in_quotes = false;
            quote_char = '\0';
        } else if (!in_quotes && c == ' ') {
            if (!current_arg.empty()) {
                result.push_back(current_arg);
                current_arg.clear();
            }
        } else {
            current_arg += c;
        }
    }

    if (!current_arg.empty()) {
        result.push_back(current_arg);
    }

    return result;
}

inline std::string validate_custom_args(const std::string& custom_args_str,
                                        const std::set<std::string>& reserved_flags) {
    std::vector<std::string> custom_args = parse_custom_args(custom_args_str);

    for (const auto& arg : custom_args) {
        std::string flag = arg;
        size_t eq_pos = flag.find('=');
        if (eq_pos != std::string::npos) {
            flag = flag.substr(0, eq_pos);
        }

        if (!flag.empty() && flag[0] == '-' && reserved_flags.find(flag) != reserved_flags.end()) {
            std::string reserved_list;
            for (const auto& reserved_flag : reserved_flags) {
                if (!reserved_list.empty()) {
                    reserved_list += ", ";
                }
                reserved_list += reserved_flag;
            }

            return "Argument '" + flag + "' is managed by Lemonade and cannot be overridden.\n"
                   "Reserved arguments: " + reserved_list;
        }
    }

    return "";
}

} // namespace utils
} // namespace lemon
