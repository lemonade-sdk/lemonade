#pragma once

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
    std::string public_recipe, install_recipe, pin_recipe;
    std::string backend, requested_version, pinned_version, resolved_version;
    PlatformInfo platform;
    InstallPhase phase;
};
PlatformInfo native_platform();

}
