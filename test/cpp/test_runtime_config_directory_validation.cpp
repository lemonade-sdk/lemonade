#include <lemon/runtime_config.h>
#include <lemon/utils/path_utils.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using lemon::RuntimeConfig;
using lemon::json;
using lemon::utils::path_to_utf8;

namespace {

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("lemonade-runtime-config-dir-validation-" + std::to_string(stamp));
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
};

bool accepts_directory_value(const fs::path& path, const char* label) {
    RuntimeConfig config(json{{"extra_models_dir", ""}});
    try {
        config.set(json{{"extra_models_dir", path_to_utf8(path)}});
        std::printf("[PASS] %s\n", label);
        return true;
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s: %s\n", label, error.what());
        return false;
    }
}

bool rejects_directory_value(const fs::path& path, const char* label) {
    RuntimeConfig config(json{{"extra_models_dir", ""}});
    try {
        config.set(json{{"extra_models_dir", path_to_utf8(path)}});
        std::printf("[FAIL] %s: value was unexpectedly accepted\n", label);
        return false;
    } catch (const std::invalid_argument&) {
        std::printf("[PASS] %s\n", label);
        return true;
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s: unexpected exception: %s\n", label, error.what());
        return false;
    }
}

} // namespace

int main() {
    TempDirectory temp;
    int failures = 0;

    const fs::path readable_dir = temp.path() / "readable";
    fs::create_directory(readable_dir);
    if (!accepts_directory_value(readable_dir, "existing readable directory is accepted")) {
        ++failures;
    }

    const fs::path missing_dir = temp.path() / "not-created";
    if (!accepts_directory_value(missing_dir, "nonexistent directory is accepted")) {
        ++failures;
    }

    const fs::path regular_file = temp.path() / "model.gguf";
    {
        std::ofstream output(regular_file);
        output << "test";
    }
    if (!rejects_directory_value(regular_file, "existing regular file is rejected")) {
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}
