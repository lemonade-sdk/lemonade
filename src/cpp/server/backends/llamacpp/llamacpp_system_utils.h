#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "llamacpp_effects.h"

#ifdef __linux__
#include <unistd.h>
#endif

namespace lemon {
namespace backends {
namespace llamacpp {
namespace detail {

// Private, deterministic pieces of the system llama.cpp HIP lookup. The full
// production availability check stays in llamacpp_server.cpp so this header is
// only a narrow test seam, not a second implementation of the policy.
inline bool is_valid_ggml_hip_plugin_path(const std::filesystem::path& path) {
#if LLAMA_LINUX
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const bool name_matches = name.rfind("libggml-hip", 0) == 0 &&
                              name.find(".so") != std::string::npos;

    std::error_code ec;
    return name_matches && LLAMA_EFFECT(std::filesystem::is_regular_file(path, ec), oracle::effects().regular(path));
#else
    (void)path;
    return false;
#endif
}

inline bool ggml_hip_env_override_available() {
#if LLAMA_LINUX
    const char* env = LLAMA_EFFECT(std::getenv("LEMONADE_GGML_HIP_PATH"), oracle::effects().environment("LEMONADE_GGML_HIP_PATH"));
    return env && *env && is_valid_ggml_hip_plugin_path(env);
#else
    return false;
#endif
}

inline bool ggml_hip_plugin_near_llama_server(const std::string& path_str) {
#if LLAMA_LINUX
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(':', start);
        std::string dir = path_str.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::path bin_dir(dir);
            std::filesystem::path llama_server = bin_dir / "llama-server";
            if (LLAMA_EFFECT(std::filesystem::is_regular_file(llama_server, ec), oracle::effects().regular(llama_server)) &&
                LLAMA_EFFECT(access(llama_server.c_str(), X_OK) == 0, oracle::effects().executable(llama_server))) {
                ec.clear();
                if (LLAMA_EFFECT(std::filesystem::exists(bin_dir / "libggml-hip.so", ec), oracle::effects().exists(bin_dir / "libggml-hip.so"))) {
                    return true;
                }
                ec.clear();
                if (LLAMA_EFFECT(std::filesystem::exists(
                        bin_dir.parent_path() / "lib" / "libggml-hip.so", ec), oracle::effects().exists(bin_dir.parent_path() / "lib" / "libggml-hip.so"))) {
                    return true;
                }
                // Preserve the current resolver policy: once PATH selects an
                // executable llama-server, do not inspect later PATH entries.
                break;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
#else
    (void)path_str;
#endif
    return false;
}

}  // namespace detail
}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
