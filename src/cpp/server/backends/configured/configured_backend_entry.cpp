#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/launch_planner.h"
#include "lemon/backends/configured/configured_server.h"

namespace lemon::backends {
namespace {
BackendDescriptor descriptor_from(const BackendDefinition& definition) {
    const auto& metadata = definition.records.at("metadata");
    BackendDescriptor descriptor;
    descriptor.recipe = definition.recipe;
    descriptor.display_name = metadata.at("display_name");
    const auto& binary = metadata.at("binary");
    descriptor.binary = binary.is_string() ? binary.get<std::string>() : binary.at(native_platform().os).get<std::string>();
    descriptor.config_section = metadata.value("config_section", "");
    const auto device = metadata.value("default_device", "gpu");
    descriptor.default_device = device == "cpu" ? DEVICE_CPU : device == "npu" ? DEVICE_NPU : device == "none" ? DEVICE_NONE : DEVICE_GPU;
    const auto policy = metadata.value("slot_policy", "standard");
    descriptor.slot_policy = policy == "exclusive_npu" ? SlotPolicy::ExclusiveNpu : policy == "coexist_by_type" ? SlotPolicy::CoexistByType : policy == "unmetered" ? SlotPolicy::Unmetered : SlotPolicy::Standard;
    descriptor.selectable_backend = metadata.value("selectable_backend", false);
    descriptor.uses_ctx_size = metadata.value("uses_ctx_size", false);
    descriptor.dynamic_models = metadata.value("dynamic_models", false);
    for (const auto& option : metadata.at("options")) descriptor.options.push_back({option["name"], option.value("cli_flag", ""), option["default_value"], option.value("type_name", ""), option.value("help", ""), option.value("group", "")});
    for (const auto& row : metadata.value("support", json::array())) {
        BackendSupport support;
        support.backend = row["backend"];
        const auto tier = row["tier"].get<std::string>();
        const auto format = row["format"].get<std::string>();
        support.tier = tier == "core" ? BackendTier::Core : tier == "experimental" ? BackendTier::Experimental : BackendTier::Guest;
        support.format = format == "native" ? BackendFormat::Native : format == "python" ? BackendFormat::Python : BackendFormat::Container;
        support.supported_os = row["supported_os"].get<std::set<std::string>>();
        support.devices = row["devices"].get<DeviceConstraints>();
        support.device_summary = row["device_summary"];
        const auto gates = row.value("arch_gates", json::object());
        for (const auto& [arch, gate] : gates.items()) support.arch_gates.emplace(arch, ArchInstallGate{gate["os"].get<std::set<std::string>>(), gate["channels"].get<std::set<std::string>>()});
        descriptor.support.push_back(std::move(support));
    }
    descriptor.supported_modes = metadata.value("supported_modes", std::vector<std::string>{"chat"});
    descriptor.required_checkpoints = metadata.value("required_checkpoints", std::vector<std::string>{"main"});
    descriptor.default_capabilities = metadata.value("default_capabilities", std::vector<std::string>{});
    descriptor.web_display_name = metadata.value("web_display_name", "");
    descriptor.rocm_channels = metadata.value("rocm_channels", std::vector<std::string>{});
    descriptor.exposes_prometheus_metrics = metadata.value("exposes_prometheus_metrics", false);
    descriptor.rocm_requires_cwsr_fix = metadata.value("rocm_requires_cwsr_fix", false);
    descriptor.version_policy = metadata.value("version_policy", "exact") == "exact" ? VersionPolicy::Exact : VersionPolicy::AtLeast;
    descriptor.self_manages_downloads = metadata.value("self_manages_downloads", false);
    descriptor.takes_args = metadata.value("takes_args", false);
    descriptor.arg_variants = metadata.value("arg_variants", std::vector<std::string>{});
    descriptor.bin_variants = metadata.value("bin_variants", std::vector<std::string>{});
    descriptor.config_extra = metadata.value("config_extra", json::object());
    descriptor.streams_model_from_storage = metadata.value("streams_model_from_storage", false);
    return descriptor;
}
}

struct ConfiguredBackendEntry::FactoryState {
    std::weak_ptr<const ConfiguredBackendEntry> owner;
};
std::shared_ptr<const ConfiguredBackendEntry> ConfiguredBackendEntry::make(std::shared_ptr<const BackendDefinition> definition, TrustedCompatibility compatibility) {
    auto entry = std::make_shared<ConfiguredBackendEntry>(std::move(definition), compatibility);
    entry->factory_state_->owner = entry;
    return entry;
}

ConfiguredBackendEntry::ConfiguredBackendEntry(std::shared_ptr<const BackendDefinition> input, TrustedCompatibility trusted)
    : factory_state_(std::make_shared<FactoryState>()), definition(std::move(input)), descriptor(descriptor_from(*definition)),
      ops(std::make_shared<ConfiguredBackendOps>(definition)),
      spec(definition->records["install"].value("install_recipe", definition->recipe), descriptor.binary,
           definition->records["install"].value("supports_split_archive", false), definition->recipe,
           definition->records["install"].value("pin_recipe", definition->recipe), definition,
           [owned_ops = ops](const InstallContext& context) { return owned_ops->install_params(context); }),
      compatibility(trusted),
      factory([state = factory_state_](const BackendContext& context) -> std::unique_ptr<WrappedServer> {
          auto owner = state->owner.lock();
          if (!owner) throw std::logic_error("Configured factory entry has expired");
          return std::make_unique<ConfiguredServer>(std::move(owner), context);
      }) {
    if (trusted != TrustedCompatibility::None && definition->origin != DefinitionOrigin::Builtin) throw std::invalid_argument("External definitions cannot attach compiled compatibility");
}

}
