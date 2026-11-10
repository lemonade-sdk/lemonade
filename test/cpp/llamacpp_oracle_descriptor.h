#pragma once

#include "lemon/backends/backend_descriptor.h"

inline nlohmann::json dump_llamacpp_descriptor(const lemon::BackendDescriptor& d) {
    using json = nlohmann::json;
    json options = json::array(), support = json::array();
    for (const auto& option : d.options) options.push_back({{"name", option.name}, {"cli_flag", option.cli_flag},
        {"default_value", option.default_value}, {"type_name", option.type_name}, {"help", option.help}, {"group", option.group}});
    for (const auto& row : d.support) {
        json gates = json::object();
        for (const auto& [arch, gate] : row.arch_gates) gates[arch] = {{"os", gate.os}, {"channels", gate.channels}};
        support.push_back({{"backend", row.backend}, {"tier", static_cast<int>(row.tier)}, {"format", static_cast<int>(row.format)},
            {"supported_os", row.supported_os}, {"devices", row.devices}, {"device_summary", row.device_summary}, {"arch_gates", gates}});
    }
    return {{"recipe", d.recipe}, {"display_name", d.display_name}, {"binary", d.binary}, {"config_section", d.config_section},
        {"default_device", static_cast<int>(d.default_device)}, {"slot_policy", static_cast<int>(d.slot_policy)},
        {"selectable_backend", d.selectable_backend}, {"uses_ctx_size", d.uses_ctx_size}, {"dynamic_models", d.dynamic_models},
        {"options", options}, {"support", support}, {"supported_modes", d.supported_modes}, {"required_checkpoints", d.required_checkpoints},
        {"default_capabilities", d.default_capabilities}, {"web_display_name", d.web_display_name}, {"rocm_channels", d.rocm_channels},
        {"exposes_prometheus_metrics", d.exposes_prometheus_metrics}, {"rocm_requires_cwsr_fix", d.rocm_requires_cwsr_fix},
        {"version_policy", static_cast<int>(d.version_policy)}, {"self_manages_downloads", d.self_manages_downloads},
        {"takes_args", d.takes_args}, {"arg_variants", d.arg_variants}, {"bin_variants", d.bin_variants}, {"config_extra", d.config_extra},
        {"streams_model_from_storage", d.streams_model_from_storage}, {"config_defaults", d.config_defaults()}};
}
