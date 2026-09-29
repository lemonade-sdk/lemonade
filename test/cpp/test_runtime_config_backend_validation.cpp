#include <lemon/runtime_config.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

using lemon::RuntimeConfig;
using nlohmann::json;

namespace {

int failures = 0;

void expect_accepted(const std::string& section, const std::string& key, const json& value) {
    RuntimeConfig config(json::object());
    try {
        config.set({{section, {{key, value}}}});
        std::printf("[PASS] %s.%s is accepted\n", section.c_str(), key.c_str());
    } catch (const std::invalid_argument& error) {
        std::printf("[FAIL] %s.%s was rejected: %s\n", section.c_str(), key.c_str(), error.what());
        ++failures;
    }
}

void expect_rejected(const std::string& section, const std::string& key, const json& value,
                     const std::string& expected_message) {
    RuntimeConfig config(json::object());
    try {
        config.set({{section, {{key, value}}}});
        std::printf("[FAIL] %s.%s was accepted\n", section.c_str(), key.c_str());
        ++failures;
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        if (message.find(expected_message) == std::string::npos) {
            std::printf("[FAIL] %s.%s rejected with unexpected message: %s\n",
                        section.c_str(), key.c_str(), message.c_str());
            ++failures;
        } else {
            std::printf("[PASS] %s.%s is rejected\n", section.c_str(), key.c_str());
        }
    }
}

void expect_unknown_key(const std::string& section, const std::string& key, const json& value) {
    expect_rejected(section, key, value, "Unknown key: '" + section + "." + key + "'");
}

}  // namespace

int main() {
    try {
        RuntimeConfig::validate_backend_choice("llamacpp", "system");
#ifdef __linux__
        std::puts("[PASS] llamacpp system backend is accepted on Linux");
#else
        std::puts("[FAIL] llamacpp system backend was accepted on an unsupported platform");
        ++failures;
#endif
    } catch (const std::invalid_argument& error) {
#ifdef __linux__
        std::printf("[FAIL] llamacpp system backend was rejected on Linux: %s\n", error.what());
        ++failures;
#else
        std::puts("[PASS] llamacpp system backend is rejected on unsupported platforms");
#endif
    }

    // _bin / _args keys must name a backend variant the section actually has.
    expect_unknown_key("flm", "flm_bin", "builtin");
    expect_unknown_key("flm", "random_bin", "builtin");
    expect_unknown_key("flm", "foo_args", "");
    expect_unknown_key("llamacpp", "vulkan_bin_extra", "builtin");
    expect_unknown_key("llamacpp", "vulkan_args_extra", "");
    expect_unknown_key("llamacpp", "npu_bin", "builtin");
    expect_unknown_key("whispercpp", "cuda_args", "");

    expect_accepted("llamacpp", "vulkan_bin", "builtin");
    expect_accepted("llamacpp", "cuda_bin", "b8664");
    expect_accepted("llamacpp", "vulkan_args", "--no-mmap");
    expect_accepted("llamacpp", "cuda_args", "--no-mmap");
    expect_accepted("llamacpp", "args", "--no-mmap");
    expect_accepted("flm", "npu_bin", "builtin");
    expect_accepted("flm", "args", "");
    expect_accepted("ryzenai", "server_bin", "latest");
    expect_accepted("hrx", "hrx_bin", "builtin");

    expect_rejected("llamacpp", "vulkan_bin", 1, "'llamacpp.vulkan_bin' must be a string");
    expect_rejected("llamacpp", "vulkan_args", 1, "'llamacpp.vulkan_args' must be a string");
    const std::string missing_bin =
        (std::filesystem::temp_directory_path() / "lemonade-does-not-exist" / "llama-server")
            .string();
    expect_rejected("llamacpp", "vulkan_bin", missing_bin,
                    "'llamacpp.vulkan_bin' path does not exist");

    return failures == 0 ? 0 : 1;
}
