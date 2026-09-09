#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace test_helpers {

inline int passed = 0;
inline int failures = 0;

inline void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("[PASS] %s\n", desc);
        ++passed;
    } else {
        std::printf("[FAIL] %s\n", desc);
        ++failures;
    }
}

inline void reset_counts() {
    passed = 0;
    failures = 0;
}

inline int report_results(const char* suite_name) {
    std::printf("================================================\n");
    if (failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", failures);
        return 1;
    } else {
        std::printf("All %s tests PASSED (%d passed).\n", suite_name, passed);
        return 0;
    }
}

// Replicate CLI parser logic to test it unit-wise
inline nlohmann::json parse_cli_args(const std::vector<std::string>& args) {
    auto normalize_key = [](std::string s) {
        std::replace(s.begin(), s.end(), '-', '_');
        return s;
    };
    auto parse_typed_value = [](const std::string& val) -> nlohmann::json {
        if (!val.empty() && (val.front() == '[' || val.front() == '{')) {
            auto parsed = nlohmann::json::parse(val, nullptr, false);
            if (!parsed.is_discarded()) {
                return parsed;
            }
        }
        if (val == "true") return true;
        if (val == "false") return false;
        try {
            size_t idx;
            int i = std::stoi(val, &idx);
            if (idx == val.size()) return i;
        } catch (...) {}
        try {
            size_t idx;
            double d = std::stod(val, &idx);
            if (idx == val.size()) return d;
        } catch (...) {}
        return val;
    };

    nlohmann::json updates = nlohmann::json::object();
    for (const auto& arg : args) {
        size_t eq_pos = arg.find('=');
        if (eq_pos == std::string::npos || eq_pos == 0) continue;
        std::string key = arg.substr(0, eq_pos);
        std::string value = arg.substr(eq_pos + 1);

        std::vector<std::string> path;
        size_t last_pos = 0;
        while (true) {
            size_t next_dot = key.find('.', last_pos);
            if (next_dot == std::string::npos) {
                std::string part = key.substr(last_pos);
                if (!part.empty()) {
                    path.push_back(normalize_key(part));
                }
                break;
            }
            std::string part = key.substr(last_pos, next_dot - last_pos);
            if (!part.empty()) {
                path.push_back(normalize_key(part));
            }
            last_pos = next_dot + 1;
        }

        if (path.empty()) continue;

        nlohmann::json* current = &updates;
        for (size_t i = 0; i < path.size(); ++i) {
            const std::string& k = path[i];
            if (i == path.size() - 1) {
                (*current)[k] = parse_typed_value(value);
            } else {
                if (!current->contains(k) || !(*current)[k].is_object()) {
                    (*current)[k] = nlohmann::json::object();
                }
                current = &((*current)[k]);
            }
        }
    }
    return updates;
}

} // namespace test_helpers
