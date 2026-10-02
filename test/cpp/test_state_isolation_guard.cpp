#include "support/test_state_isolation.h"

#include "lemon/utils/path_utils.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool is_within(const fs::path& child, const fs::path& root) {
    if (child == root) {
        return true;
    }
    const fs::path relative = child.lexically_relative(root);
    if (relative.empty()) {
        return false;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

bool check_inside(const char* label, const std::string& value, const fs::path& root) {
    const fs::path path = lemon::utils::path_from_utf8(value).lexically_normal();
    if (is_within(path, root.lexically_normal())) {
        return true;
    }
    std::cerr << label << " escaped test sandbox: " << value << '\n';
    return false;
}

} // namespace

int main() {
    const fs::path root = lemon::test::state_dir();

    bool ok = true;
    ok &= check_inside("cache", lemon::utils::get_cache_dir(), root);
    ok &= check_inside("config", lemon::utils::get_config_dir(), root);

    // Tests such as config-dir migration intentionally exercise default path
    // resolution. Clearing an override must not expose the developer's real dirs.
    lemon::utils::set_cache_dir("");
    lemon::utils::set_config_dir("");

    ok &= check_inside("cache fallback", lemon::utils::get_cache_dir(), root);
    ok &= check_inside("config fallback", lemon::utils::get_config_dir(), root);

    if (!ok) {
        return 1;
    }
    std::cout << "state isolation guard passed\n";
    return 0;
}
