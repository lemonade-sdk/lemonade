#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace extensor {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "extensor",
    /*display_name*/    "EXTENSOR ROCm (experimental)",
    /*binary*/          "extensor-server",
    /*config_section*/  "",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"extensor_backend", "--extensor", "", "BACKEND",
         "EXTENSOR backend to use", "EXTENSOR Options"},
        {"extensor_model_path", "--extensor-model-path", "", "PATH",
         "Path to an EXTENSOR model image", "EXTENSOR Options"},
        {"extensor_preset", "--extensor-preset", "balanced", "PRESET",
         "EXTENSOR runtime preset: exact, balanced, fast, or demo", "EXTENSOR Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151", "gfx1152", "gfx1201"}}},
         "AMD ROCm GPUs (gfx1151, gfx1152, gfx1201)"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {"reasoning", "tool-calling"},
    /*experimental*/    true,
    /*web_display_name*/ "EXTENSOR ROCm",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"rocm"},
    /*config_extra*/    {{"extensor_model_path", ""}, {"extensor_preset", "balanced"}},
    /*streams_model_from_storage*/ true,
    /*skip_model_size_filter*/ true,
};

}  // namespace extensor
}  // namespace backends
}  // namespace lemon
