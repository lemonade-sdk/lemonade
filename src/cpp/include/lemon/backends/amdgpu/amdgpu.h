#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace amdgpu {

// amdgpu-server runs ONNX Runtime GenAI models built for the HIP execution
// provider. They are not the NPU models ryzenai-llm runs, so they get their
// own recipe rather than a ryzenai-llm backend.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "amdgpu-llm",
    /*display_name*/    "AMD GPU LLM",
#ifdef _WIN32
    /*binary*/          "amdgpu-server.exe",
#else
    /*binary*/          "amdgpu-server",
#endif
    /*config_section*/  "amdgpu",  // differs from recipe "amdgpu-llm"
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // gpu is the only backend
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"gpu", BackendTier::Experimental, BackendFormat::Native, {"windows"}, {{"amd_gpu", {"gfx1151"}}}, "AMD Strix Halo"},
    },
    /*about*/ {
        {"gpu", {"AMD's ONNX Runtime GenAI stack running LLMs on Radeon GPUs through the HIP execution provider.",
                 "AMD", "lemonade-sdk", "MIT (server) and AMD EULA (AMD GPU runtime libraries)",
                 "https://github.com/lemonade-sdk/amdgpu-server"}},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"gpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace amdgpu
}  // namespace backends
}  // namespace lemon
