#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace gufo {

// Arguments gufo_args must not carry: they would move the port out from under
// the proxy, rebind the host, rename the model the router forwards to, or swap
// the weights and drafts the model's checkpoints name.
inline const std::set<std::string>& reserved_custom_arg_flags() {
    static const std::set<std::string> kReserved = {
        "-m", "--model",
        "-c", "--context",
        "--mmproj",
        "--served-model-name",
        "--speculative", "--dflash-model", "--dspark-model", "--mtp-model",
        "-i", "--host",
        "-p", "--port",
        "--api-key",
    };
    return kReserved;
}

// Gufo is an inference engine with model-specific gfx1151 kernels. It reads
// ordinary GGUFs but runs only the few models it was written for, so they get
// their own recipe rather than a llamacpp backend.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "gufo",
    /*display_name*/    "Gufo",
    /*binary*/          "gufo",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only backend
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"gufo_args", "--gufo-args", "", "ARGS",
         "Custom arguments to pass to gufo serve llm", "Gufo Options"},
    },
    /*support*/ {
        {"rocm", BackendTier::Guest, BackendFormat::Container, {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "AMD Strix Halo"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "Gufo",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ true,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
    /*streams_model_from_storage*/ false,
    /*containers*/ {
        {"rocm", {
            /*image*/   "docker.io/kyuz0/amd-strix-halo-toolboxes",
            /*devices*/ {"/dev/dri", "/dev/kfd"},
            /*cap_add*/ {},
        }},
    },
};

}  // namespace gufo
}  // namespace backends
}  // namespace lemon
