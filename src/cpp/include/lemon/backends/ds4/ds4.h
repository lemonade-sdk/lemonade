#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace ds4 {

// Arguments ds4_args must not carry. ds4-server parses left-to-right, so a
// later flag wins over the ones Lemonade sets: moving the port breaks readiness
// and proxying, rebinding the host exposes the backend, swapping the model
// desynchronises the router, and the backend-selection flags would run the
// child on a different device than the one Lemonade is tracking.
inline const std::set<std::string>& reserved_custom_arg_flags() {
    static const std::set<std::string> flags = {
        "-m", "--model",
        "--host", "--port",
        "-c", "--ctx",
        "--backend", "--cpu", "--metal", "--rocm", "--cuda",
    };
    return flags;
}

// The ds4 backend descriptor (plain data). DS4 (DwarfStar) is antirez's
// self-contained DeepSeek V4 inference engine with an OpenAI-compatible HTTP
// server (ds4-server). It runs from Donato Capitella's Strix Halo DS4 image.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ds4",
    /*display_name*/    "DwarfStar4 (experimental)",
    /*binary*/          "ds4-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only variant; no option to declare
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"ds4_args", "--ds4-args", "", "ARGS",
         "Custom arguments to pass to ds4-server", "DS4 Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "Prebuilt ds4 for AMD Strix Halo"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
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
    /*streams_model_from_storage*/ true,
    /*labels*/ {
        {"rocm", {BackendTier::Experimental, BackendFormat::Container}},
    },
    /*containers*/ {
        {"rocm", {
            /*repository*/        "docker.io/kyuz0/strix-halo-ds4-toolbox",
            /*devices*/           {"/dev/dri", "/dev/kfd"},
            /*cap_add*/           {"SYS_PTRACE"},
            /*ipc_host*/          true,
            /*memlock_unlimited*/ false,
        }},
    },
};

}  // namespace ds4
}  // namespace backends
}  // namespace lemon
