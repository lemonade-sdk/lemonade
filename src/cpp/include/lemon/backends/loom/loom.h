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
    /*display_name*/    "Loom",
    /*binary*/          "loom-serve",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,  // the bundle's preset fixes the context capacity
    /*dynamic_models*/  false,
    /*options*/ {
        {"loom_device", "--loom-device", "amdgpu://", "URI",
         "HAL device URI loom-serve runs the bundle on. amdgpu:// is amdgpu://0, the first GPU "
         "loom-serve supports in its own enumeration order (not a DRM card or HIP device index); "
         "amdgpu://1 selects the next one",
         "Loom Options"},
        {"loom_memory_bytes", "--loom-memory-bytes", "", "BYTES",
         "Device pool size, e.g. 16gib, served as an elastic pool; empty serves the bundle "
         "as its card does, with a fixed pool sized by the card's preset",
         "Loom Options"},
        {"loom_args", "--loom-args", "", "ARGS",
         "Custom arguments to pass to loom-serve", "Loom Options"},
    },
    /*support*/ {
        // gfx120X because Windows names the R9700 and RX 9070 by family only; load() turns
        // away the gfx1200 parts the family also covers.
        {"rocm", BackendTier::Experimental, BackendFormat::Native, {"windows", "linux"},
         {{"amd_gpu", {"gfx1151", "gfx120X"}}},
         "AMD Strix Halo (gfx1151), Radeon gfx1201 32 GB (AI PRO R9700), Radeon gfx1201 16 GB (RX 9070 XT, RX 9070)",
         {{"gfx1151", {/*os*/ {"windows", "linux"}, /*channels*/ {}}},
          {"gfx120X", {/*os*/ {"windows", "linux"}, /*channels*/ {}}}}},
    },
    /*about*/ {
        {"rocm", {"AMD's experimental HRX Loom runtime, serving compiled Loom model bundles.",
                 "AMD Loom", "AMD Loom", "Apache-2.0", "https://github.com/ROCm/hrx-system"}},
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
    /*bin_variants*/    {"rocm"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace loom
}  // namespace backends
}  // namespace lemon
