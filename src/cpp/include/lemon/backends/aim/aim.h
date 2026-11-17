#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace aim {

// AMD Inference Microservices: vLLM images AMD publishes per model. Every
// Radeon AIM is the aim-radeon-base image plus that model's tuned profiles, so
// the backend pins the base and each model row names its own image in
// `aim_image`, which then costs only those few layers to pull.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "aim",
    /*display_name*/    "AMD Inference Microservice",
    /*binary*/          "",  // the image's entrypoint picks a profile and starts vLLM
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only backend
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"aim_profile", "--aim-profile", "", "PROFILE",
         "AIM profile to serve with, instead of the one AIM picks for this GPU", "AIM Options"},
    },
    /*support*/ {
        {"rocm", BackendTier::Guest, BackendFormat::Container, {"linux"}, {{"amd_gpu", {"gfx1201"}}},
         /*device_summary*/ "AMD Radeon AI PRO R9700"},
    },
    /*about*/ {
        {"rocm", {"AMD's per-model vLLM microservices, each with profiles tuned for Radeon PRO GPUs.",
                    "AMD", "AMD (amdenterpriseai)", "Apache-2.0, MIT",
                    "https://enterprise-ai.docs.amd.com/en/latest/aims/overview.html"}},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "AIM",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
    /*streams_model_from_storage*/ false,
    /*containers*/ {
        {"rocm", {
            /*image*/   "docker.io/amdenterpriseai/aim-radeon-base",
            /*devices*/ {"/dev/dri", "/dev/kfd"},
            /*cap_add*/ {},
            /*user*/    std::nullopt,
            // Every AIM profile runs vLLM's multiprocess executor, even on one
            // GPU, and its message queues take about 400 MiB of /dev/shm.
            /*shm_size*/ "1g",
        }},
    },
};

}  // namespace aim
}  // namespace backends
}  // namespace lemon
