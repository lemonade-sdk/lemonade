#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/configured/configured_backend_ops.h"
#include "lemon/server_process.h"

namespace lemon::backends {

enum class TrustedCompatibility { None, LegacyNonChatSse };

struct BuiltinLoadLog {
    std::function<void(const std::string& category, const std::string& model_name)> loading;
    std::function<void(const std::string& category, const std::string& phase, const json& values)> prepared;
    std::function<void(const std::string& category, const json& values, const ServerCommand& command)> starting;
};

struct BuiltinLaunchCompatibility {
    std::optional<DeviceType> device_on_backend_selection;
    bool posix_executable_loader_path = false;
    std::optional<BuiltinLoadLog> load_log;
};

std::vector<OperationBinding> configured_operation_bindings(const BackendDefinition& definition, TrustedCompatibility trusted);

struct ConfiguredBackendEntry {
private:
    struct FactoryState;
    const std::shared_ptr<FactoryState> factory_state_;
public:
    static std::shared_ptr<const ConfiguredBackendEntry> make(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility = TrustedCompatibility::None, BuiltinLaunchCompatibility launch_compatibility = {});
    explicit ConfiguredBackendEntry(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility = TrustedCompatibility::None, BuiltinLaunchCompatibility launch_compatibility = {});
    ConfiguredBackendEntry(const ConfiguredBackendEntry&) = delete;
    ConfiguredBackendEntry& operator=(const ConfiguredBackendEntry&) = delete;
    const std::shared_ptr<const BackendDefinition> definition;
    const BackendDescriptor descriptor;
    const std::vector<OperationBinding> bindings;
    const std::shared_ptr<const ConfiguredBackendOps> ops;
    const BackendSpec spec;
    const TrustedCompatibility compatibility;
    const BuiltinLaunchCompatibility launch_compatibility;
    const BackendCreateFn factory;
};

}
