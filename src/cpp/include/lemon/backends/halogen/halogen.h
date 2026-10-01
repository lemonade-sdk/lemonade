#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace halogen {

// Halogen Flash is Peonist's inference engine with hand-written gfx1151
// kernels. It serves one model family, Qwen3.8-Flash-Next, as an HGN checkpoint
// plus an overlay, from its own image, and takes its settings as HALOGEN_*
// environment variables rather than a command line.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "halogen",
    /*display_name*/    "Halogen Flash (experimental)",
    /*binary*/          "",  // the image's entrypoint serves the model
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only backend
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, /*device_summary*/ "AMD Strix Halo"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "Halogen Flash",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
    // The HGN checkpoint is mapped read-only and registered with the GPU in
    // place, so what must fit in the GPU's pool is each model's
    // min_resident_gb, not the checkpoint's size.
    /*streams_model_from_storage*/ true,
    /*labels*/ {
        {"rocm", {BackendTier::Experimental, BackendFormat::Container}},
    },
    /*containers*/ {
        {"rocm", {
            /*image*/   "ghcr.io/peonist-ai/halogen-flash-server",
            /*devices*/ {"/dev/dri", "/dev/kfd"},
            /*cap_add*/ {},
        }},
    },
};

}  // namespace halogen
}  // namespace backends
}  // namespace lemon
