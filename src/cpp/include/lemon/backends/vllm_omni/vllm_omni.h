#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace vllm_omni {

// The vllm-omni backend descriptor (plain data). Header-only `inline const` so
// it links into both the lemonade CLI and lemond without a separate source file.
//
// vLLM-Omni serves omni / any-to-any multimodal models (Qwen-Omni today) with
// ROCm acceleration. It is a pure-Python layer on top of the same base vLLM +
// PyTorch + Triton the plain `vllm` backend uses, shipped as a SEPARATE release
// artifact (vllm-omni*), so it gets its own recipe + version pin. gfx1151 only:
// that is the qualified, hardware-validated omni target.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "vllm-omni",
    /*display_name*/    "vLLM-Omni ROCm (experimental)",
    /*binary*/          "vllm-omni-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"vllm-omni_backend", "--vllm-omni", "", "BACKEND",
         "vLLM-Omni backend to use", "vLLM-Omni Options"},
        {"vllm_omni_args", "--vllm-omni-args", "", "ARGS",
         "Custom arguments to pass to vllm-omni-server", "vLLM-Omni Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "Strix Halo iGPU (gfx1151)"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Omni (text, audio, vision)",
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

}  // namespace vllm_omni
}  // namespace backends
}  // namespace lemon
