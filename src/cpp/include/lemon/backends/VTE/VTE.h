#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace VTE {

// Header-only `inline const` so it links into both the CLI and lemond
// without a separate source file. Folder/namespace is "VTE" (uppercase) to
// stay visually distinct from the lowercase-stem backends; `recipe` itself
// stays lowercase, consistent with every other recipe identifier.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "vte",
    /*display_name*/    "VTE (RDNA3 native, experimental)",
    /*binary*/          "vte-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // single HIP-only flavor, nothing to select between
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"rocm", {"windows"}, {{"amd_gpu", {"gfx110X"}}}, "RDNA3 native, validated on RX 7600; RX 7700/7800/7900 series untested"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    true,  // opt-in: must not compete for VRAM with an unselected backend
    /*web_display_name*/ "",
    /*rocm_channels*/   {"stable"},  // non-empty on purpose: this is what triggers TheRock
                                      // (the HIP runtime) install alongside vte-server
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,  // no Triton/ROCm-PyTorch kernel path; unconfirmed on
                                        // real hardware whether the gfx1151 CWSR issue applies
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"rocm"},  // single internal flavor string; selectable_backend=false
    // ctx_size cannot default here via config_extra: RuntimeConfig::recipe_options()
    // only reads a recipe's own `options`, and its ctx_size special case only reads
    // the global top-level key, never a per-recipe one (confirmed by testing, not
    // assumed). The default lives in server_models.json's recipe_options instead.
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace VTE
}  // namespace backends
}  // namespace lemon
