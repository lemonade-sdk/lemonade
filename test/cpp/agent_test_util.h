// Shared helpers for the agent host unit tests.
#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace agent_test {

inline int failures = 0;

inline void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

inline int finish() {
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

// A fresh directory; TMPDIR is honoured since /tmp may be noexec.
inline std::filesystem::path temp_dir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lemonade-" + name + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

inline void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

} // namespace agent_test
