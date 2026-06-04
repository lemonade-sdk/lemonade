#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lemon {
namespace utils {

inline std::vector<std::string> parse_custom_args(const std::string& custom_args_str, bool keep_quotes = false) {
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
            if (keep_quotes) {
                current_arg = quote_char + current_arg + quote_char;
            }
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

inline std::map<std::string, std::vector<std::string>> build_custom_args_map(const std::vector<std::string>& tokens) {
    std::map<std::string, std::vector<std::string>> result;
    std::string last_flag;  // Track the most recently seen flag independently of map ordering

    for (const auto& token : tokens) {
        if (!token.empty() && token[0] == '-') {
            // This is a flag; start a new entry
            result[token] = {};
            last_flag = token;
        } else if (!last_flag.empty()) {
            // Append to the most recently seen flag
            result[last_flag].push_back(token);
        }
    }

    return result;
}

inline std::string validate_custom_args(const std::string& custom_args_str, const std::set<std::string>& reserved_flags) {
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

inline std::string map_to_args_string(const std::map<std::string, std::vector<std::string>>& m) {
    std::string result;
    bool first = true;
    for (const auto& [flag, values] : m) {
        if (!first) result += " ";
        first = false;
        result += flag;
        for (const auto& v : values) {
            result += " " + v;
        }
    }
    return result;
}

// Given a flag like "--flag" or "--no-flag", return the negation key.
// "--no-<name>" ↔ "--<name>". Returns empty string if no negation exists.
inline std::string negate_flag(const std::string& flag) {
    if (flag.size() >= 5 && flag.compare(0, 5, "--no-") == 0) {
        return "--" + flag.substr(5);
    }
    if (flag.size() >= 3 && flag.compare(0, 2, "--") == 0) {
        return "--no-" + flag.substr(2);
    }
    return "";
}

inline std::map<std::string, std::vector<std::string>> merge_args_maps(
    const std::map<std::string, std::vector<std::string>>& target,
    const std::map<std::string, std::vector<std::string>>& incoming) {
    std::map<std::string, std::vector<std::string>> merged = target;

    // Remove binary-flag negations from incoming that conflict with target.
    // Only flags without arguments are considered binary flags.
    for (const auto& [flag, values] : incoming) {
        if (values.empty()) {
            std::string neg = negate_flag(flag);
            if (!neg.empty() && merged.count(neg)) {
                // Target has the opposite binary flag — skip this incoming flag
                continue;
            }
        }
        if (!merged.count(flag)) {
            merged[flag] = values;
        }
    }
    return merged;
}

} // namespace utils
} // namespace lemon
