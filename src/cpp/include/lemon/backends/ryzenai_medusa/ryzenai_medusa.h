#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace ryzenai_medusa {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "ryzenai-llm-medusa",
    /*display_name*/    "Ryzen AI LLM (Medusa)",
#ifdef _WIN32
    /*binary*/          "ryzenai-server.exe",
#else
    /*binary*/          "ryzenai-server",
#endif
    /*config_section*/  "ryzenai_medusa",
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"npu", {"windows"}, {{"amd_npu", {"XDNA3"}}}, "XDNA3 NPU (Medusa)"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    false,
    /*web_display_name*/ "Ryzen AI SW NPU (Medusa)",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"server"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace ryzenai_medusa
}  // namespace backends
}  // namespace lemon
