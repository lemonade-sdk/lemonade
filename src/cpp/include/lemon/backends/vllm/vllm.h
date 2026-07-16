#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace vllm {

// The vllm backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "vllm",
    /*display_name*/    "vLLM ROCm (experimental)",
    /*binary*/          "vllm-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"vllm_backend", "--vllm", "", "BACKEND",
         "vLLM backend to use", "vLLM Options"},
        {"vllm_args", "--vllm-args", "", "ARGS",
         "Custom arguments to pass to vllm-server", "vLLM Options"},
    },
    /*support*/ {
        // gfx942 omitted until its vLLM/ROCm asset ships in lemonade-sdk/vllm-rocm;
        // everything else is wired, so re-add it here once that lands.
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1150", "gfx1151", "gfx110X", "gfx120X"}}}, "Strix Halo iGPU (gfx1151)"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},  // single rocm artifact, no stable/nightly channels
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace vllm
}  // namespace backends
}  // namespace lemon
