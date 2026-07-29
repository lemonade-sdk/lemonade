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
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"extensor_backend", "--extensor", "", "BACKEND",
         "EXTENSOR backend to use", "EXTENSOR Options"},
        {"extensor_model_path", "--extensor-model-path", "", "PATH",
         "Path to an EXTENSOR model image", "EXTENSOR Options"},
        {"extensor_preset", "--extensor-preset", "demo", "PRESET",
         "EXTENSOR runtime preset: exact, demo, or fast", "EXTENSOR Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "Strix Halo iGPU (gfx1151)"},
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
    /*self_manages_downloads*/ true,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"rocm"},
    /*config_extra*/    {{"extensor_model_path", ""}, {"extensor_preset", "demo"}},
    /*streams_model_from_storage*/ true,
};

}  // namespace extensor
}  // namespace backends
}  // namespace lemon
