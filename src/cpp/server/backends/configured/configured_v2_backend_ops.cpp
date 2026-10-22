#include "backend_builtin_entry_internal.h"
#include "configured_v2_backend_ops_internal.h"
#include "lemon/backends/configured/backend_definition_descriptor.h"
#include "lemon/backends/configured/backend_spec_utils.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/configured_server.h"

namespace lemon::backends {

bool configured_external_only(const BackendSpec& spec) {
    if (spec.definition && spec.executable_definition) throw std::logic_error("Ambiguous configured spec ownership");
    return spec.definition ? spec.definition->records["install"]["strategy"] == "external_only" :
        spec.executable_definition && spec.executable_definition->compiled().normalized_declaration()["install"]["strategy"] == "external_only";
}
std::string configured_external_default_version(const BackendSpec& spec, const std::string& backend, const json& shipped_pins) {
    if (spec.definition && spec.executable_definition) throw std::logic_error("Ambiguous configured spec ownership");
    const json* records = nullptr;
    if (spec.definition && spec.definition->origin == DefinitionOrigin::External) records = &spec.definition->records;
    if (spec.executable_definition && spec.executable_definition->compiled().authorized().definition().key.space == DefinitionNamespace::External)
        records = &spec.executable_definition->compiled().normalized_declaration();
    if (!records) return "";
    const auto pinned = shipped_pins.value(spec.effective_pin_recipe(), json::object());
    if (pinned.contains(backend) && pinned[backend].is_string()) return pinned[backend].get<std::string>();
    const auto versions = records->at("install").value("default_versions", json::object());
    return versions.contains(backend) ? versions[backend].get<std::string>() : "";
}
InstallContext configured_install_context(const BackendSpec& spec, const std::string& backend, const std::string& requested,
    const std::string& pinned, PlatformInfo platform) {
    const auto public_recipe = spec.public_recipe.empty() ? spec.recipe : spec.public_recipe;
    return {spec, spec.definition, public_recipe, spec.pin_recipe.empty() ? public_recipe : spec.pin_recipe,
        backend, requested, pinned, "", std::move(platform), InstallPhase::Repository, spec.executable_definition};
}
ConfiguredInstallResolution configured_install_params(const BackendSpec& spec, const std::string& backend,
    const std::string& requested, const std::string& pinned, PlatformInfo platform,
    const std::function<std::string(const std::string&)>& resolve_version) {
    if (!spec.install_params_fn) throw std::runtime_error("No install params function for recipe: " + spec.recipe);
    auto context = configured_install_context(spec, backend, requested, pinned, std::move(platform));
    const auto repository = spec.install_params_fn(context);
    context.resolved_version = resolve_version(repository.repo);
    context.phase = InstallPhase::Asset;
    auto asset = spec.install_params_fn(context);
    auto version = asset.version_override.empty() ? context.resolved_version : asset.version_override;
    return {std::move(asset), std::move(version)};
}

namespace {
bool same_json_fields(const json& left, const json& right) {
    if (left.type() != right.type() || left.size() != right.size()) return false;
    if (left.is_object()) {
        for (const auto& [key, value] : left.items()) {
            const auto found = right.find(key);
            if (found == right.end() || !same_json_fields(value, *found)) return false;
        }
        return true;
    }
    if (left.is_array()) {
        for (std::size_t i = 0; i < left.size(); ++i) if (!same_json_fields(left[i], right[i])) return false;
        return true;
    }
    return left == right;
}

json descriptor_fields(const BackendDescriptor& d) {
    json options = json::array(), support = json::array();
    for (const auto& option : d.options) options.push_back({
        {"name", option.name}, {"cli_flag", option.cli_flag}, {"default_value", option.default_value},
        {"type_name", option.type_name}, {"help", option.help}, {"group", option.group}});
    for (const auto& row : d.support) {
        json gates = json::object();
        for (const auto& [arch, gate] : row.arch_gates) gates[arch] = {{"os", gate.os}, {"channels", gate.channels}};
        support.push_back({{"backend", row.backend}, {"tier", static_cast<int>(row.tier)},
            {"format", static_cast<int>(row.format)}, {"supported_os", row.supported_os}, {"devices", row.devices},
            {"device_summary", row.device_summary}, {"arch_gates", gates}});
    }
    return {
        {"recipe", d.recipe},
        {"display_name", d.display_name},
        {"binary", d.binary},
        {"config_section", d.config_section},
        {"default_device", static_cast<int>(d.default_device)},
        {"slot_policy", static_cast<int>(d.slot_policy)},
        {"selectable_backend", d.selectable_backend},
        {"uses_ctx_size", d.uses_ctx_size},
        {"dynamic_models", d.dynamic_models},
        {"options", options},
        {"support", support},
        {"supported_modes", d.supported_modes},
        {"required_checkpoints", d.required_checkpoints},
        {"default_capabilities", d.default_capabilities},
        {"web_display_name", d.web_display_name},
        {"rocm_channels", d.rocm_channels},
        {"exposes_prometheus_metrics", d.exposes_prometheus_metrics},
        {"rocm_requires_cwsr_fix", d.rocm_requires_cwsr_fix},
        {"version_policy", static_cast<int>(d.version_policy)},
        {"self_manages_downloads", d.self_manages_downloads},
        {"takes_args", d.takes_args},
        {"arg_variants", d.arg_variants},
        {"bin_variants", d.bin_variants},
        {"config_extra", d.config_extra},
        {"streams_model_from_storage", d.streams_model_from_storage}
    };
}
}

struct ConfiguredBackendEntry::V2FactoryState {
    std::weak_ptr<const ConfiguredBackendEntry> owner;
};
std::shared_ptr<const ConfiguredBackendEntry> ConfiguredBackendEntry::make(std::shared_ptr<const ExecutableDefinition> executable) {
    auto entry = std::shared_ptr<ConfiguredBackendEntry>(new ConfiguredBackendEntry(std::move(executable)));
    entry->v2_factory_state_->owner = entry;
    return entry;
}

ConfiguredBackendEntry::ConfiguredBackendEntry(std::shared_ptr<const ExecutableDefinition> executable, const BackendDescriptor* canonical)
    : v2_factory_state_(std::make_shared<V2FactoryState>()), canonical_descriptor_(canonical), runner_(std::make_shared<const DefinitionRunner>(executable)),
      descriptor(descriptor_from_declaration(runner_->declaration(), executable->compiled().authorized().definition().key.space, DescriptorPlatform::native())),
      bindings(runner_->operation_bindings()), v2_ops(std::make_shared<const ConfiguredV2BackendOps>(runner_)),
      executable_definition(std::move(executable)), spec(descriptor.recipe, descriptor.binary, executable_definition,
          [runner = runner_](const InstallContext& context) { return runner->install_params(context); },
          runner_->declaration()["install"].value("supports_split_archive", false),
          executable_definition->compiled().authorized().definition().key.space == DefinitionNamespace::Builtin &&
          executable_definition->compiled().authorized().definition().key.id == "llamacpp" &&
          executable_definition->compiled().authorized().definition().key.contract == 1 && descriptor.recipe == "llamacpp"),
      compatibility(TrustedCompatibility::None),
      factory([state = v2_factory_state_](const BackendContext& context) -> std::unique_ptr<WrappedServer> {
          auto owner = state->owner.lock();
          if (!owner) throw std::logic_error("Configured factory entry has expired");
          return std::make_unique<ConfiguredServer>(std::move(owner), context);
      }) {
    if (canonical && (executable_definition->compiled().authorized().definition().key.space != DefinitionNamespace::Builtin ||
        !same_json_fields(descriptor_fields(*canonical), descriptor_fields(descriptor))))
        throw std::logic_error("Canonical builtin descriptor differs from resolved declaration");
}

std::shared_ptr<const ConfiguredBackendEntry> BuiltinEntryBuilder::make(
    std::shared_ptr<const ExecutableDefinition> executable, const BackendDescriptor& canonical) {
    auto entry = std::shared_ptr<ConfiguredBackendEntry>(new ConfiguredBackendEntry(std::move(executable), &canonical));
    entry->v2_factory_state_->owner = entry;
    return entry;
}

const json& ConfiguredBackendEntry::records() const {
    return definition ? definition->records : executable_definition->compiled().normalized_declaration();
}


void ConfiguredV2BackendOps::resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const {
    runner_->resolve_runtime_options(model, options);
}
InstallParams ConfiguredV2BackendOps::install_params(const InstallContext& context) const {
    return runner_->install_params(context);
}
std::string ConfiguredV2BackendOps::resolve_version(const std::string& backend, const std::string& version) const {
    return runner_->resolve_version(backend, version);
}
BackendOps::InstallCheck ConfiguredV2BackendOps::check_install(const std::string& backend, bool found) const {
    return runner_->check_install(backend, found);
}
std::string ConfiguredV2BackendOps::resolve_checkpoint_path(const ModelInfo&, const CheckpointResolveContext& context) const {
    return runner_->resolve_artifact(context);
}
bool ConfiguredV2BackendOps::is_downloaded(const ModelInfo& model, const BackendOpsContext& context) const {
    return BackendOps::is_downloaded(model, context);
}
void ConfiguredV2BackendOps::download_model(const ModelInfo& model, bool do_not_upgrade, DownloadProgressCallback progress, const BackendOpsContext& context) const {
    if (!context.model_manager) throw std::invalid_argument("V2 registry download requires model manager");
    BackendOps::download_model(model, do_not_upgrade, std::move(progress), context);
}
void ConfiguredV2BackendOps::populate_metadata(ModelInfo& info, const BackendOpsContext&) const { runner_->populate_metadata(info); }
std::string ConfiguredV2BackendOps::validate_registration_checkpoint(const std::string& checkpoint) const { return runner_->validate_registration(checkpoint); }
std::string ConfiguredV2BackendOps::validate_checkpoint_file(const std::string& path) const { return runner_->validate_file(path); }
std::string ConfiguredV2BackendOps::find_imported_checkpoint(const std::string& path) const { return runner_->find_imported(path); }

}
