#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

#include "test_config_helpers.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using lemon::BackendManager;
using lemon::RuntimeConfig;
using lemon::SystemInfo;
using lemon::backends::BackendUtils;
using test_helpers::check;

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("lemonade-extensor-install-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const char* previous_override = std::getenv("LEMONADE_EXTENSOR_ROCM_BIN");
    const bool had_override = previous_override != nullptr;
    const std::string saved_override = had_override ? previous_override : "";
#ifdef _WIN32
    _putenv_s("LEMONADE_EXTENSOR_ROCM_BIN", "");
#else
    unsetenv("LEMONADE_EXTENSOR_ROCM_BIN");
#endif
    lemon::utils::set_cache_dir(root.string());

    try {
        // If a download guard regresses, an unsupported ISA prevents an actual network request.
        SystemInfo::set_rocm_arch_override("unsupported-test-arch");
        for (const std::string key : {"offline", "no_fetch_executables"}) {
            RuntimeConfig config({{"offline", false}, {"no_fetch_executables", false},
                                  {"extensor", {{"rocm_bin", "builtin"}}}});
            config.set({{key, true}});
            RuntimeConfig::set_global(&config);
            BackendManager manager;
            bool blocked = false;
            try {
                manager.install_backend("extensor", "rocm");
            } catch (const std::exception& error) {
                const std::string message = error.what();
                blocked = message.find(key == "offline" ? "offline mode" :
                                       "Fetching executable artifacts is disabled") != std::string::npos;
            }
            check(blocked, "download guard rejects a missing EXTENSOR binary before URL resolution");

            const fs::path install_dir = BackendUtils::get_install_directory("extensor", "rocm");
            const fs::path binary = install_dir / "extensor-v2.3.2" / "bin" / "extensor-server";
            fs::create_directories(binary.parent_path());
            std::ofstream(binary) << "cached binary sentinel";
            std::ofstream(install_dir / "version.txt") << "v2.3.2";
            bool completed = false;
            manager.install_backend("extensor", "rocm", false,
                [&completed](const lemon::DownloadProgress& progress) {
                    completed = progress.complete;
                    return true;
                });
            check(completed, "download guard allows an existing managed binary");
            check(BackendUtils::get_backend_binary_path(
                      *lemon::backends::try_get_spec_for_recipe("extensor"), "rocm") == binary.string(),
                  "runtime resolves the cached EXTENSOR executable inside the archive directory");
            bool forced_blocked = false;
            try {
                manager.install_backend("extensor", "rocm", true);
            } catch (const std::exception& error) {
                const std::string message = error.what();
                forced_blocked = message.find(key == "offline" ? "offline mode" :
                                              "Fetching executable artifacts is disabled") != std::string::npos;
            }
            check(forced_blocked, "force cannot bypass a download guard");
            fs::remove_all(install_dir);
            RuntimeConfig::set_global(nullptr);
        }

#ifdef __linux__
        RuntimeConfig config({{"offline", true}, {"no_fetch_executables", true},
                              {"extensor", {{"rocm_bin", "builtin"}}}});
        RuntimeConfig::set_global(&config);
        BackendManager manager;
        for (const std::string arch : {"gfx1151", "gfx1152", "gfx1201"}) {
            SystemInfo::set_rocm_arch_override(arch);
            const auto params = manager.get_install_params("extensor", "rocm");
            check(params.repo.empty() && params.download_url.find("https://www.amd.com/") == 0,
                  "manager propagates the direct AMD URL");
            check(params.filename.find(arch + ".zip") != std::string::npos,
                  "manager preserves the concrete GPU target");
            check(SystemInfo::backend_supports_arch("extensor", "rocm", arch),
                  "descriptor advertises the published target");
            check(manager.get_release_url("extensor", "rocm") == params.download_url,
                  "backend status points at AMD");
            check(manager.get_or_resolve_latest_tag("extensor", "rocm").empty(),
                  "fixed AMD releases have no GitHub latest lookup");
        }
        check(!SystemInfo::backend_supports_arch("extensor", "rocm", "gfx1200"),
              "descriptor does not include the whole gfx120X family");
        config.set({{"extensor", {{"rocm_bin", "2.3.2"}}}});
        const auto unprefixed = manager.get_install_params("extensor", "rocm");
        check(unprefixed.version == "2.3.2" &&
                  unprefixed.filename == "extensor-v2.3.2-linux-x86_64-rocm7-gfx1201.zip",
              "version tracking retains an unprefixed pin while selecting the AMD asset");
        config.set({{"extensor", {{"rocm_bin", "latest"}}}});
        bool latest_rejected = false;
        try {
            manager.get_install_params("extensor", "rocm");
        } catch (const std::invalid_argument& error) {
            latest_rejected = std::string(error.what()).find("use 'builtin'") != std::string::npos;
        }
        check(latest_rejected, "latest reports that a fixed AMD version is required");
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Unexpected exception: %s\n", error.what());
        check(false, "installation policy test completed");
    }

    RuntimeConfig::set_global(nullptr);
    SystemInfo::set_rocm_arch_override("");
    lemon::utils::set_cache_dir("");
#ifdef _WIN32
    _putenv_s("LEMONADE_EXTENSOR_ROCM_BIN", saved_override.c_str());
#else
    if (had_override) setenv("LEMONADE_EXTENSOR_ROCM_BIN", saved_override.c_str(), 1);
    else unsetenv("LEMONADE_EXTENSOR_ROCM_BIN");
#endif
    fs::remove_all(root);
    return test_helpers::report_results("EXTENSOR backend installation");
}
