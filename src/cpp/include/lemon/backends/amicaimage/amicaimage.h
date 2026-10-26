#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace amicaimage {

// The amica-image backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
// It runs the same amica-engine and AMICA install as the amica recipe, so it
// shares that recipe's config.json section; a recipe of its own is needed
// because a backend serving chat cannot also serve images.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "amica-image",
    /*display_name*/    "AMICA DX12 Image",
    /*binary*/          "amica-driver",
    /*config_section*/  "amica",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"amica_args", "--amica-args", "", "ARGS",
         "Custom arguments to pass to amica-engine (--adapter, --arch, --weights-vram-mib)",
         "AMICA Options"},
    },
    /*support*/ {
        {"dx12", BackendTier::Experimental, BackendFormat::Native, {"windows"},
         {{"amd_gpu", {"gfx110X", "gfx1150", "gfx1151", "gfx120X"}}},
         "AMD RDNA3/RDNA4 GPUs (DX12)"},
    },
    /*supported_modes*/ {"image"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "AMICA",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {"dx12"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace amicaimage
}  // namespace backends
}  // namespace lemon
