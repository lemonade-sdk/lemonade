#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace ryzenai {

// The ryzenai backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ryzenai-llm",
    /*display_name*/    "Ryzen AI LLM",
#ifdef _WIN32
    /*binary*/          "ryzenai-server.exe",
#else
    /*binary*/          "ryzenai-server",
#endif
    /*config_section*/  "ryzenai",  // differs from recipe "ryzenai-llm"
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"npu", {"windows"}, {{"amd_npu", {"XDNA2"}}}, "XDNA2 NPU"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    false,
    /*web_display_name*/ "Ryzen AI SW NPU",
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

}  // namespace ryzenai
}  // namespace backends
}  // namespace lemon
