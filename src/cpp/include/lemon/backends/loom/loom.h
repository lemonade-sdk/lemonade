#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace loom {

// loom-serve takes the last occurrence of a flag, so a custom argument naming one
// of these would move the listener off the port Lemonade proxies to, expose it
// beyond loopback, swap the bundle behind the router, or run it on a device
// Lemonade is not tracking. A flagfile could carry any of them.
inline const std::set<std::string>& reserved_custom_arg_flags() {
    static const std::set<std::string> flags = {
        "--flagfile",
        "--model",
        "--listen",
        "--device",
        "--devices",
        "--memory_bytes",
        "--preset",
    };
    return flags;
}

inline const BackendDescriptor descriptor = {
    /*recipe*/          "loom",
    /*display_name*/    "HRX Loom",
    /*binary*/          "loom-serve",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,  // the bundle's preset fixes the context capacity
    /*dynamic_models*/  false,
    /*options*/ {
        {"loom_device", "--loom-device", "amdgpu://", "URI",
         "HAL device URI loom-serve runs the bundle on", "Loom Options"},
        {"loom_memory_bytes", "--loom-memory-bytes", "", "BYTES",
         "Device pool size, e.g. 16gib; empty uses the bundle's card", "Loom Options"},
        {"loom_args", "--loom-args", "", "ARGS",
         "Custom arguments to pass to loom-serve", "Loom Options"},
    },
    /*support*/ {
        {"amdgpu", BackendTier::Experimental, BackendFormat::Native, {"windows", "linux"},
         {{"amd_gpu", {"gfx1151"}}},
         "AMD Strix Halo (gfx1151)",
         {{"gfx1151", {/*os*/ {"windows", "linux"}, /*channels*/ {}}}}},
    },
    /*about*/ {
        {"amdgpu", {"AMD's experimental HRX Loom runtime, serving compiled Loom model bundles.",
                    "AMD ROCm", "AMD ROCm", "Apache-2.0", "https://github.com/ROCm/hrx-system"}},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ true,  // a bundle pulls GGUFs from the repos its card pins
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {"amdgpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace loom
}  // namespace backends
}  // namespace lemon
