#pragma once

#include <string>
#include <vector>

namespace lemon::utils {

inline std::string escape_windows_arg(const std::string& arg) {
    if (arg.empty()) {
        return "\"\"";
    }

    bool needs_quotes = false;
    bool has_cmd_meta = false;
    for (char c : arg) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
            needs_quotes = true;
        }
        if (c == '&' || c == '|' || c == '^' || c == '%' || c == '<' || c == '>' || c == '(' || c == ')') {
            has_cmd_meta = true;
        }
    }

    if (!needs_quotes && !has_cmd_meta) {
        return arg;
    }

    if (!needs_quotes && has_cmd_meta) {
        std::string escaped;
        for (char c : arg) {
            if (c == '&' || c == '|' || c == '^' || c == '%' || c == '<' || c == '>' || c == '(' || c == ')') {
                escaped.push_back('^');
            }
            escaped.push_back(c);
        }
        return escaped;
    }

    // CommandLineToArgvW standard escaping
    std::string base_arg = "\"";
    int backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            base_arg.append(backslashes * 2 + 1, '\\');
            base_arg.push_back('"');
            backslashes = 0;
        } else {
            base_arg.append(backslashes, '\\');
            base_arg.push_back(c);
            backslashes = 0;
        }
    }
    base_arg.append(backslashes * 2, '\\');
    base_arg.push_back('"');

    if (!has_cmd_meta) {
        return base_arg;
    }

    // Process cmd.exe quote state and metacharacters
    std::string escaped;
    bool in_cmd_quotes = false;
    for (size_t i = 0; i < base_arg.length(); ++i) {
        char c = base_arg[i];
        if (c == '"') {
            size_t backslash_count = 0;
            size_t j = i;
            while (j > 0 && base_arg[j - 1] == '\\') {
                backslash_count++;
                j--;
            }
            if (backslash_count % 2 == 0) {
                in_cmd_quotes = !in_cmd_quotes;
            }
            escaped.push_back(c);
        } else if (c == '%') {
            if (in_cmd_quotes) {
                escaped += "\"^%\"";
            } else {
                escaped += "^%";
            }
        } else if (!in_cmd_quotes && (c == '&' || c == '|' || c == '^' || c == '<' || c == '>' || c == '(' || c == ')')) {
            escaped.push_back('^');
            escaped.push_back(c);
        } else {
            escaped.push_back(c);
        }
    }
    return escaped;
}

inline std::string escape_posix_shell_arg(const std::string& arg) {
    if (arg.empty()) {
        return "''";
    }
    bool safe = true;
    for (char c : arg) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' && c != '/' && c != ':') {
            safe = false;
            break;
        }
    }
    if (safe) {
        return arg;
    }

    std::string escaped = "'";
    for (char c : arg) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += "'";
    return escaped;
}

inline std::string escape_shell_arg(const std::string& arg) {
#ifdef _WIN32
    return escape_windows_arg(arg);
#else
    return escape_posix_shell_arg(arg);
#endif
}

inline std::string sanitize_log_string(const std::string& input) {
    std::string result = input;
    static const std::vector<std::string> sensitive_keywords = {"KEY", "TOKEN", "SECRET", "PASS", "AUTH"};
    for (const auto& kw : sensitive_keywords) {
        size_t pos = 0;
        while ((pos = result.find(kw, pos)) != std::string::npos) {
            size_t eq_pos = result.find('=', pos);
            if (eq_pos != std::string::npos && eq_pos - pos < 40) {
                size_t val_end = result.find_first_of(" \t\n\r&", eq_pos + 1);
                if (val_end == std::string::npos) val_end = result.length();
                result.replace(eq_pos + 1, val_end - (eq_pos + 1), "[REDACTED]");
            }
            pos += kw.length();
        }
    }
    return result;
}

} // namespace lemon::utils
