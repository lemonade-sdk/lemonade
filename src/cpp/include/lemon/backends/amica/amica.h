#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace amica {

// The amica backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "amica",
    /*display_name*/    "AMICA DX12",
    // Locates the AMICA tools directory amica.dx12_bin names. Generation runs
    // in amica-engine against that install's amica.dll, not in this driver.
    /*binary*/          "amica-driver",
    /*config_section*/  "",  // defaults to recipe
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
    /*supported_modes*/ {"chat"},
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

}  // namespace amica
}  // namespace backends
}  // namespace lemon
