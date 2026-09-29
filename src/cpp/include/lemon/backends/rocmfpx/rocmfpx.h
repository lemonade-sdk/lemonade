#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace rocmfpx {

// Arguments rocmfpx_args must not carry. llama-server parses left to right, so
// a later copy of any of these would move the port out from under the proxy,
// rebind the host, or swap the model the router is tracking.
inline const std::set<std::string>& reserved_custom_arg_flags() {
    static const std::set<std::string> kReserved = {
        "-c", "--ctx-size",
        "-m", "--model",
        "-mu", "--model-url",
        "-hf", "-hfr", "-hff", "--hf-repo", "--hf-file",
        "-md", "--model-draft",
        "-mm", "--mmproj",
        "--host", "--port",
        "--jinja", "--no-jinja",
        "--metrics",
    };
    return kReserved;
}

// ROCm FPX is a llama.cpp fork that adds the ROCmFP4, FP6 and FP8 weight
// formats and MTP drafting. Mainline llama.cpp cannot read those weights, so its
// models run on this recipe alone. It runs from the rocm-10.0-rocmfpx build of
// Donato Capitella's Strix Halo toolboxes.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "rocmfpx",
    /*display_name*/    "ROCm FPX (experimental)",
    /*binary*/          "llama-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only backend
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"rocmfpx_args", "--rocmfpx-args", "", "ARGS",
         "Custom arguments to pass to the ROCm FPX llama-server", "ROCm FPX Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "AMD Strix Halo"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "ROCm FPX",
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
    /*labels*/ {
        {"rocm", {BackendTier::Experimental, BackendFormat::Container}},
    },
    /*containers*/ {
        {"rocm", {
            /*repository*/        "docker.io/kyuz0/amd-strix-halo-toolboxes",
            /*devices*/           {"/dev/dri", "/dev/kfd"},
            /*cap_add*/           {},
            /*ipc_host*/          false,
            /*memlock_unlimited*/ false,
        }},
    },
};

}  // namespace rocmfpx
}  // namespace backends
}  // namespace lemon
