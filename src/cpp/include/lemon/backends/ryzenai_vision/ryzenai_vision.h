#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace ryzenai_vision {

// The ryzenai-vision backend descriptor (plain data). Runs ONNX vision models on
// the NPU through the VitisAI execution provider of a separately installed Ryzen
// AI Software environment, so the "binary" is that environment's Python
// interpreter (located via ryzenaivision.npu_bin or RYZEN_AI_INSTALLATION_PATH)
// and nothing is downloaded or version-pinned by lemonade.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ryzenai-vision",
    /*display_name*/    "Ryzen AI Vision",
#ifdef _WIN32
    /*binary*/          "python.exe",
#else
    /*binary*/          "python",
#endif
    /*config_section*/  "ryzenaivision",
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"ryzenaivision_args", "--ryzenaivision-args", "", "ARGS",
         "Custom arguments to pass to ryzenai_vision_server.py", "Ryzen AI Vision Options"},
    },
    /*support*/ {
        {"npu", BackendTier::Experimental, BackendFormat::Python, {"windows", "linux"},
         {{"amd_npu", {"XDNA2"}}}, "XDNA2 NPU"},
    },
    /*supported_modes*/ {"detection"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "Ryzen AI SW Vision NPU",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {"npu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace ryzenai_vision
}  // namespace backends
}  // namespace lemon
