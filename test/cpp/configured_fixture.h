#pragma once

#include "backend_definition_resources.h"
#include "lemon/backends/backend_descriptor.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/launch_planner.h"
#include "lemon/utils/aixlog.hpp"
#include "thinksound_oracle.h"

namespace thinksound_oracle {

struct LogCapture {
    std::vector<std::string> records;
    explicit LogCapture(const std::string& tag) {
        AixLog::Log::init<AixLog::SinkCallback>(AixLog::Severity::trace, [this, tag](const AixLog::Metadata& metadata, const std::string& message) {
            if (metadata.tag.text == tag) records.push_back(AixLog::to_string(metadata.severity) + " " + message);
        });
    }
    ~LogCapture() { AixLog::Log::init({}); }
};

inline std::shared_ptr<const backends::ConfiguredBackendEntry> entry(const std::string& text = backends::k_thinksound_definition, backends::TrustedCompatibility compatibility = backends::TrustedCompatibility::None) {
    const auto result = backends::BackendDefinitionStore().validate({"fixture.json", text, backends::DefinitionOrigin::Builtin});
    if (!result.definition) {
        for (const auto& error : result.diagnostics) std::printf("%s %s\n", error.code.c_str(), error.pointer.c_str());
        throw std::runtime_error("Invalid configured fixture");
    }
    return backends::ConfiguredBackendEntry::make(result.definition, compatibility);
}
inline const BackendDescriptor& registered_descriptor(const std::string& recipe) {
    const BackendDescriptor* registered = backends::descriptor_for(recipe);
    if (!registered) throw std::runtime_error("No registered descriptor for " + recipe);
    return *registered;
}
inline void descriptor(const BackendDescriptor& value, const BackendDescriptor& compiled, bool gated = false) {
    check(value.recipe == compiled.recipe && value.display_name == compiled.display_name && value.binary == compiled.binary && value.config_section == compiled.config_section, "compiled descriptor identities");
    check(value.default_device == compiled.default_device && value.slot_policy == compiled.slot_policy && value.selectable_backend == compiled.selectable_backend && value.uses_ctx_size == compiled.uses_ctx_size && value.dynamic_models == compiled.dynamic_models, "compiled descriptor policy");
    check(value.supported_modes == compiled.supported_modes && value.required_checkpoints == compiled.required_checkpoints && value.default_capabilities == compiled.default_capabilities && value.web_display_name == compiled.web_display_name && value.rocm_channels == compiled.rocm_channels, "compiled descriptor lists");
    check(value.exposes_prometheus_metrics == compiled.exposes_prometheus_metrics && value.rocm_requires_cwsr_fix == compiled.rocm_requires_cwsr_fix && value.version_policy == compiled.version_policy && value.self_manages_downloads == compiled.self_manages_downloads && value.takes_args == compiled.takes_args && value.arg_variants == compiled.arg_variants && value.bin_variants == compiled.bin_variants && value.config_extra == compiled.config_extra && value.streams_model_from_storage == compiled.streams_model_from_storage, "compiled descriptor flags");
    check(value.config_defaults() == compiled.config_defaults() && value.options.size() == compiled.options.size() && value.support.size() == compiled.support.size(), "compiled descriptor defaults/options/support counts");
    for (size_t i = 0; i < value.options.size(); ++i) {
        const auto& a = value.options[i]; const auto& b = compiled.options[i];
        check(a.name == b.name && a.cli_flag == b.cli_flag && a.default_value == b.default_value && a.type_name == b.type_name && a.help == b.help && a.group == b.group, "every compiled option field");
    }
    for (size_t i = 0; i < value.support.size(); ++i) {
        const auto& a = value.support[i]; const auto& b = compiled.support[i];
        bool gates = a.arch_gates.size() == b.arch_gates.size() && (gated ? !a.arch_gates.empty() : a.arch_gates.empty());
        for (const auto& [arch, gate] : a.arch_gates) gates = gates && b.arch_gates.count(arch) && b.arch_gates.at(arch).os == gate.os && b.arch_gates.at(arch).channels == gate.channels;
        check(a.backend == b.backend && a.tier == b.tier && a.format == b.format && a.supported_os == b.supported_os && a.devices == b.devices && a.device_summary == b.device_summary && gates, "every compiled support field");
    }
}
class FixtureLaunchServices : public backends::EffectLaunchServices {
public:
    explicit FixtureLaunchServices(EffectsFixture& fixture) : backends::EffectLaunchServices(fixture.effects()), effects_(fixture.effects()), external_(fixture.external) {}
    bool exists(const std::string& path) const override {
        return path == external_ && !external_.empty() ? effects_.binary.exists(path) : effects_.exists(path);
    }
private:
    backends::BackendEffects effects_;
    std::string external_;
};

}
