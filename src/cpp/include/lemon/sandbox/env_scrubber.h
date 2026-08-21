#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lemon::sandbox {

class EnvScrubber {
public:
    static bool is_sensitive_key(const std::string& key);

    static bool is_allowlisted_key(const std::string& key);

    static std::vector<std::pair<std::string, std::string>> get_ambient_environment();

    static std::vector<std::pair<std::string, std::string>> sanitize_environment(
        const std::vector<std::pair<std::string, std::string>>& custom_env = {},
        const std::vector<std::string>& extra_allowlist = {},
        bool include_ambient = true);

    static const std::unordered_set<std::string>& default_allowlist();

    static const std::unordered_set<std::string>& sensitive_exact_keys();

    static const std::vector<std::string>& sensitive_prefixes();

    static const std::vector<std::string>& sensitive_suffixes();
};

} // namespace lemon::sandbox
