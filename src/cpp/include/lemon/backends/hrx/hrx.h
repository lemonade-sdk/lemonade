#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace hrx {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "llamacpp-hrx",
    /*display_name*/    "HRX GPU",
    /*binary*/          "llama-server",
    /*config_section*/  "hrx",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"hrx_args", "--hrx-args", "", "ARGS",
         "Custom arguments to pass to the HRX llama-server", "HRX Options"},
    },
    /*support*/ {
        {"hrx", BackendTier::Experimental, BackendFormat::Native, {"linux"},
         {{"amd_gpu", {"gfx1100", "gfx1151"}}},
         "AMD GPUs (gfx1100, gfx1151)",
         {{"gfx1100", {/*os*/ {"linux"}, /*channels*/ {}}},
          {"gfx1151", {/*os*/ {"linux"}, /*channels*/ {}}}}},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ true,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {"hrx"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace hrx
}  // namespace backends
}  // namespace lemon
