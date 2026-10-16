#pragma once

#include <filesystem>
#include <string>

namespace lemon {
namespace test {

// Scratch root that get_cache_dir() and get_config_dir() resolve to for the
// lifetime of a test process. add_cpp_ci_test() links this into every test
// binary, so no test has to opt in and none can reach the developer's real
// ~/.config/lemonade. Removed on exit.
const std::filesystem::path& state_dir();

// A fresh empty directory under state_dir(), for tests that need fixture files
// on disk. Cleaned up with the state dir.
std::filesystem::path make_scratch_dir(const std::string& prefix);

} // namespace test
} // namespace lemon
