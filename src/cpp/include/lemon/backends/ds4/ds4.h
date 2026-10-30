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

// On an APU the GPU pool is the desktop's memory too. Left alone, ds4-server
// grows its caches until its own free check reads 16 GiB, but that check misses
// its model tensor cache and streaming buffers, so on a 62 GiB Strix Halo the
// pool really fell to 15 GB free and the compositor died with the login
// session. The free floor and tensor cache cap here kept 25 GB free under a
// long prompt at full prefill speed. Auto-tune reads the same plan to keep the
// context out of that floor.
struct MemoryPlan {
    int expert_cache_gb = 0;  // --ssd-streaming-cache-experts
    int model_cache_gb = 0;   // DS4_ROCM_STREAM_MODEL_CACHE_GB
    int free_reserve_gb = 0;  // DS4_ROCM_STREAM_FREE_RESERVE_GB
};

inline MemoryPlan memory_plan(double pool_gb) {
    MemoryPlan plan;
    if (pool_gb <= 0.0) return plan;
    plan.expert_cache_gb = static_cast<int>(pool_gb * 0.4);
    plan.model_cache_gb = static_cast<int>(pool_gb * 0.2);
    plan.free_reserve_gb = static_cast<int>(pool_gb * 0.6);
    return plan;
}

// The ds4 backend descriptor (plain data). DS4 (DwarfStar) is antirez's
// self-contained DeepSeek V4 inference engine with an OpenAI-compatible HTTP
// server (ds4-server). It runs from Donato Capitella's Strix Halo DS4 image.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ds4",
    /*display_name*/    "DwarfStar4",
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
        {"rocm", BackendTier::Guest, BackendFormat::Container, {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "Prebuilt ds4 for AMD Strix Halo"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
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
    /*containers*/ {
        {"rocm", {
            /*image*/   "docker.io/kyuz0/strix-halo-ds4-toolbox",
            /*devices*/ {"/dev/dri", "/dev/kfd"},
            /*cap_add*/ {"SYS_PTRACE"},
        }},
    },
};

}  // namespace ds4
}  // namespace backends
}  // namespace lemon
