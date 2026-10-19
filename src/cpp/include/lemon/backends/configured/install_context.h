#pragma once

#include <functional>
#include <memory>
#include <string>

namespace lemon::backends {

struct BackendDefinition;
struct BackendSpec;
struct PlatformInfo { std::string os, machine_arch, rocm_arch; };
enum class InstallPhase { Repository, Asset };
struct InstallContext {
    const BackendSpec& spec;
    std::shared_ptr<const BackendDefinition> definition;
    std::string public_recipe, pin_recipe;
    std::string backend, requested_version, pinned_version, resolved_version;
    PlatformInfo platform;
    InstallPhase phase;
};
struct InstallPlatformSources {
    std::function<std::string()> os;
    std::function<std::string()> rocm_arch;
};
PlatformInfo native_platform();
PlatformInfo install_platform(const BackendSpec& spec, const std::string& backend, const InstallPlatformSources& sources);

}
