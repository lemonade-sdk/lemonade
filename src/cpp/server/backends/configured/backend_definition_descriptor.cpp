#include "backend_definition_v2_internal.h"
#include "lemon/backends/configured/backend_definition_descriptor.h"

#include <map>

namespace lemon::backends {
namespace {
template <class T>
T enum_value(const std::string& name, const std::map<std::string, T>& values) {
    const auto found = values.find(name);
    if (found == values.end()) throw std::invalid_argument("Unknown descriptor enum");
    return found->second;
}
}

DescriptorPlatform DescriptorPlatform::native() {
#ifdef _WIN32
    return {"windows"};
#elif defined(__APPLE__)
    return {"macos"};
#else
    return {"linux"};
#endif
}

BackendDescriptor descriptor_from_declaration(const nlohmann::json& declaration, DefinitionNamespace origin, const DescriptorPlatform& platform) {
    if (platform.os != "linux" && platform.os != "windows" && platform.os != "macos") throw std::invalid_argument("Unknown descriptor platform");
    const auto& metadata = declaration.at("metadata");
    BackendDescriptor descriptor;
    descriptor.recipe = declaration.at("recipe").get<std::string>();
    descriptor.display_name = metadata.at("display_name");
    const auto& binary = metadata.at("binary");
    descriptor.binary = binary.is_string() ? binary.get<std::string>() : binary.at(platform.os).get<std::string>();
    descriptor.config_section = metadata.value("config_section", "");
    const auto device = metadata.value("default_device", "gpu");
    descriptor.default_device = enum_value<DeviceType>(device, {{"cpu", DEVICE_CPU}, {"gpu", DEVICE_GPU}, {"npu", DEVICE_NPU}, {"none", DEVICE_NONE}});
    const auto policy = metadata.value("slot_policy", "standard");
    descriptor.slot_policy = enum_value<SlotPolicy>(policy, {{"standard", SlotPolicy::Standard}, {"exclusive_npu", SlotPolicy::ExclusiveNpu}, {"coexist_by_type", SlotPolicy::CoexistByType}, {"unmetered", SlotPolicy::Unmetered}});
    descriptor.selectable_backend = metadata.value("selectable_backend", false);
    descriptor.uses_ctx_size = metadata.value("uses_ctx_size", false);
    descriptor.dynamic_models = metadata.value("dynamic_models", false);
    for (const auto& option : metadata.value("options", nlohmann::json::array())) descriptor.options.push_back({option["name"], option.value("cli_flag", ""), option["default_value"], option.value("type_name", ""), option.value("help", ""), option.value("group", "")});
    for (const auto& row : metadata.value("support", nlohmann::json::array())) {
        BackendSupport support;
        support.backend = row["backend"];
        const auto tier = row["tier"].get<std::string>();
        const auto format = row["format"].get<std::string>();
        support.tier = enum_value<BackendTier>(tier, {{"core", BackendTier::Core}, {"experimental", BackendTier::Experimental}, {"guest", BackendTier::Guest}});
        support.format = enum_value<BackendFormat>(format, {{"native", BackendFormat::Native}, {"python", BackendFormat::Python}, {"container", BackendFormat::Container}});
        support.supported_os = row["supported_os"].get<std::set<std::string>>();
        support.devices = row["devices"].get<DeviceConstraints>();
        support.device_summary = row["device_summary"];
        const auto gates = row.value("arch_gates", nlohmann::json::object());
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
    descriptor.version_policy = enum_value<VersionPolicy>(metadata.value("version_policy", "exact"), {{"exact", VersionPolicy::Exact}, {"at_least", VersionPolicy::AtLeast}});
    descriptor.self_manages_downloads = metadata.value("self_manages_downloads", false);
    descriptor.takes_args = metadata.value("takes_args", false);
    descriptor.arg_variants = metadata.value("arg_variants", std::vector<std::string>{});
    descriptor.bin_variants = metadata.value("bin_variants", std::vector<std::string>{});
    descriptor.config_extra = metadata.value("config_extra", nlohmann::json::object());
    descriptor.streams_model_from_storage = metadata.value("streams_model_from_storage", false);
    if (origin == DefinitionNamespace::External) for (const auto& option : metadata.value("options", nlohmann::json::array())) {
        const std::string name = option["name"];
        const std::string key = v2::config_key(name);
        descriptor.config_extra[key] = option["default_value"];
    }
    return descriptor;
}

}
