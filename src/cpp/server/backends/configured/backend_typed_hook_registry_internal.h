#pragma once

#include "backend_hook_types.h"

#include <variant>

namespace lemon::backends::typed {

using Target = std::variant<BackendSelectionOps, InstallContextHook, RuntimeOptionsHook,
    PrepareHook, EnvironmentHook, SystemOps, ArtifactOps, RequestTransform, ResponseTransform>;

struct Registration {
    Contract contract;
    std::string id;
    std::string stage;
    Target target;
    unsigned abi_revision;
    unsigned implementation_revision;
    std::vector<Effect> effects;
    std::vector<Fact> requires;
    std::vector<Fact> provides;
};

const std::vector<Registration>& installed_targets();
void audit_targets(const std::vector<Registration>& entries);

namespace llamacpp {
BackendSelection select_backend(const BackendSelectionInput&, BackendSelectionServices&);
DeviceCheck validate_device(const DeviceValidationInput&, BackendSelectionServices&);
InstallFacts install_context(const InstallFactsInput&, InstallFactsServices&);
RuntimeFacts runtime_options(const RuntimeOptionsInput&);
ModelFacts prepare(const PrepareInput&);
EnvironmentPlan environment(const EnvironmentInput&, EnvironmentServices&);
std::string resolve_version(const SystemVersionInput&, SystemProbeServices&);
SystemInstallCheck check_install(const SystemInstallInput&, SystemProbeServices&);
ArtifactMetadata metadata(const ArtifactMetadataInput&, ArtifactReadServices&);
std::string validate_registration(const std::string&);
std::string validate_file(const ArtifactFileInput&, ArtifactReadServices&);
std::string resolve_path(const ArtifactResolveInput&, ArtifactReadServices&);
std::string find_imported(const ArtifactImportInput&, ArtifactReadServices&);
nlohmann::json max_tokens_alias(nlohmann::json);
nlohmann::json tool_schema_limits(nlohmann::json);
nlohmann::json client_model(nlohmann::json, const nlohmann::json&, const std::string&);
}

#ifdef LEMONADE_TYPED_HOOK_TEST
void observe_entry(const char* entry);
#define LEMON_TYPED_ENTRY(name) ::lemon::backends::typed::observe_entry(name)
#else
#define LEMON_TYPED_ENTRY(name) ((void)0)
#endif

}
