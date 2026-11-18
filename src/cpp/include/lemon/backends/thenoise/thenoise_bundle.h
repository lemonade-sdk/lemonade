#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {
namespace thenoise_bundle {

// The Windows thenoise bundle is a portable CPython install plus a
// `thenoise.bat` shim that prepares the ROCm/Torch environment and then execs
// the bundled interpreter. Launching the shim makes cmd.exe our direct child
// and python.exe its grandchild, so stopping the server kills only cmd.exe and
// leaves python.exe behind holding the GPU. Launch the bundled interpreter
// directly instead; everything here reproduces what the shim does.

// A launch description for the bundled interpreter.
struct Launch {
    std::string executable;
    // Interpreter arguments; go ahead of the thenoise CLI arguments.
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env_vars;
};

inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

inline std::string join_path_list(const std::vector<std::string>& entries) {
    std::string joined;
    for (const std::string& entry : entries) {
        if (entry.empty()) continue;
        if (!joined.empty()) joined += ';';
        joined += entry;
    }
    return joined;
}

inline std::vector<std::pair<std::string, std::string>> env_vars_for(
    const std::string& bundle_root,
    const std::string& inherited_path) {
    namespace fs = std::filesystem;

    const fs::path root(bundle_root);
    const fs::path site_packages = root / "Lib" / "site-packages";
    const fs::path rocm_core = site_packages / "_rocm_sdk_core";

    std::vector<std::string> path_entries = {
        (rocm_core / "bin").string(),
        (rocm_core / "lib").string(),
        (rocm_core / "lib" / "llvm" / "lib").string(),
        (site_packages / "_rocm_sdk_libraries" / "bin").string(),
        (site_packages / "torch" / "lib").string(),
        site_packages.string(),
        root.string(),
        inherited_path,
    };

    return {
        {"PATH", join_path_list(path_entries)},
        {"TORCH_ROCM_AOTRITON_ENABLE_EXPERIMENTAL", "1"},
        {"MIOPEN_FIND_MODE", "FAST"},
        {"TORCH_BLAS_PREFER_HIPBLASLT", "1"},
        {"TORCH_COMPILE_DISABLE", "1"},
        {"TORCHDYNAMO_DISABLE", "1"},
    };
}

inline std::string bundle_python_exe(const std::string& launcher_path) {
    namespace fs = std::filesystem;

    const fs::path launcher(launcher_path);
    const fs::path root = launcher.parent_path();

    if (iequals(launcher.filename().string(), "python.exe")) {
        return launcher.string();
    }

    const fs::path python = root / "python.exe";
    std::error_code ec;
    return fs::exists(python, ec) ? python.string() : std::string();
}

inline std::string bundle_root_of(const std::string& launcher_path) {
    return std::filesystem::path(launcher_path).parent_path().string();
}

inline Launch make_launch(const std::string& launcher_path,
                          const std::string& inherited_path) {
    Launch launch;
    launch.executable = bundle_python_exe(launcher_path);
    if (launch.executable.empty()) {
        throw std::runtime_error(
            "python.exe not found next to the thenoise launcher '" + launcher_path +
            "'. The Windows thenoise bundle must ship its own CPython interpreter "
            "(reinstall the thenoise backend).");
    }
    launch.args = {"-s", "-m", "thenoise"};
    launch.env_vars = env_vars_for(bundle_root_of(launcher_path), inherited_path);
    return launch;
}

}  // namespace thenoise_bundle
}  // namespace backends
}  // namespace lemon
