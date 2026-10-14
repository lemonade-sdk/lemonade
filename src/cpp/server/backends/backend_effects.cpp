#include "lemon/backend_manager.h"
#include "lemon/backends/backend_effects.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"

#include <cstdlib>
#include <filesystem>

namespace lemon::backends {

BackendEffects production_backend_effects(BackendManager* manager, std::function<int()> allocate_port) {
    BackendEffects effects;
    effects.exists = [](const std::string& path) { return std::filesystem::exists(path); };
    effects.binary = {
        BackendUtils::find_external_backend_binary,
        effects.exists,
        [manager](const std::string& recipe, const std::string& backend) { manager->install_backend(recipe, backend); },
        BackendUtils::get_backend_binary_path
    };
    effects.supported_backends = [](const std::string& recipe) { return SystemInfo::get_supported_backends(recipe).backends; };
    effects.validate_backend = RuntimeConfig::validate_backend_choice;
    effects.allocate_port = std::move(allocate_port);
    effects.rocm_arch = SystemInfo::get_rocm_arch;
    effects.runtime_directories = [](const std::string& arch) {
        return BackendUtils::join_runtime_dirs(BackendUtils::get_therock_lib_paths(arch));
    };
    effects.inherited_loader = []() -> std::optional<std::string> {
#ifdef _WIN32
        const char* value = std::getenv("PATH");
#else
        const char* value = std::getenv("LD_LIBRARY_PATH");
#endif
        return value ? std::optional<std::string>(value) : std::nullopt;
    };
    effects.append_cuda = [](auto& env, const std::string& category) { BackendUtils::apply_cuda_env_vars(env, category); };
    effects.make_process = [](ProcessOutput output, const std::string& cwd) { return std::make_unique<NativeProcess>(output, cwd); };
    return effects;
}

}
