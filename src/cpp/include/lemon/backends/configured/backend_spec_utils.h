#pragma once

#include "lemon/backends/backend_utils.h"

#include <nlohmann/json.hpp>

namespace lemon::backends {

bool configured_external_only(const BackendSpec& spec);
std::string configured_external_default_version(const BackendSpec& spec, const std::string& backend, const nlohmann::json& shipped_pins);
InstallContext configured_install_context(const BackendSpec& spec, const std::string& backend, const std::string& requested,
    const std::string& pinned, PlatformInfo platform);
struct ConfiguredInstallResolution {
    InstallParams asset;
    std::string version;
};
ConfiguredInstallResolution configured_install_params(const BackendSpec& spec, const std::string& backend,
    const std::string& requested, const std::string& pinned, PlatformInfo platform,
    const std::function<std::string(const std::string&)>& resolve_version);

}
