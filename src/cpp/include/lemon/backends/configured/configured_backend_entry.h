#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/configured/configured_backend_ops.h"

namespace lemon::backends {

enum class TrustedCompatibility { None, LegacyNonChatSse };

struct ConfiguredBackendEntry {
private:
    struct FactoryState;
    const std::shared_ptr<FactoryState> factory_state_;
public:
    static std::shared_ptr<const ConfiguredBackendEntry> make(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility = TrustedCompatibility::None);
    explicit ConfiguredBackendEntry(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility = TrustedCompatibility::None);
    ConfiguredBackendEntry(const ConfiguredBackendEntry&) = delete;
    ConfiguredBackendEntry& operator=(const ConfiguredBackendEntry&) = delete;
    const std::shared_ptr<const BackendDefinition> definition;
    const BackendDescriptor descriptor;
    const std::shared_ptr<const ConfiguredBackendOps> ops;
    const BackendSpec spec;
    const TrustedCompatibility compatibility;
    const BackendCreateFn factory;
};

}
