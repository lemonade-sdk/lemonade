#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace strata {

inline const BackendDescriptor descriptor = {
    /*recipe*/ "strata",
    /*display_name*/ "Strata",
    /*binary*/ "strata",
    /*config_section*/ "strata",
    /*default_device*/ DEVICE_GPU,
    /*slot_policy*/ SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/ true,
    /*dynamic_models*/ false,
    /*options*/ {
        {"strata_backend", "--strata", "", "BACKEND",
         "Strata backend to use", "Strata Options"},
    },
    /*support*/ {
#if (defined(__linux__) || defined(_WIN32)) && (defined(__x86_64__) || defined(_M_X64))
        {"cuda", BackendTier::Experimental, BackendFormat::Python, {"linux", "windows"},
         {{"nvidia_gpu", {"sm_75", "sm_86", "sm_89", "sm_120"}}}, "NVIDIA RTX 20/30/40/50"},
        {"rocm", BackendTier::Experimental, BackendFormat::Python, {"linux", "windows"},
         {{"amd_gpu", {"gfx1100", "gfx1101", "gfx1102", "gfx1151", "gfx1200", "gfx1201"}}},
         "AMD RX 7600/7700/7800/7900, RX 9060/9070, AI PRO R9700, Strix Halo"},
#endif
    },
    /*about*/ {
        {"cuda", {"Strata's Qwen3.6 GGUF inference engine with its bundled Python HTTP server.",
                  "Niko1221", "Niko1221", "MIT", "https://github.com/Niko1221/Strata"}},
        {"rocm", {"Strata's Qwen3.6 GGUF inference engine with its bundled Python HTTP server.",
                  "Niko1221", "Niko1221", "MIT", "https://github.com/Niko1221/Strata"}},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "Strata",
    /*rocm_channels*/ {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/ VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/ false,
    /*arg_variants*/ {},
    /*bin_variants*/ {"cuda", "rocm"},
};

}  // namespace strata
}  // namespace backends
}  // namespace lemon
