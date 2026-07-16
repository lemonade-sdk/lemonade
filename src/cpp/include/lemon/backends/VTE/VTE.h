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
    // vte-server has no VRAM/device-sharing awareness of its own (its preflight is
    // disabled under Lemonade, see VTE_server.cpp), so it cannot safely coexist
    // with another GPU-resident model. ExclusiveGpu makes the router evict any
    // other GPU server before loading VTE, and evict VTE when a later GPU load
    // comes in.
    /*slot_policy*/     SlotPolicy::ExclusiveGpu,
    /*selectable_backend*/ false,  // single HIP-only flavor, nothing to select between
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"rocm", {"windows"}, {{"amd_gpu", {"gfx110X"}}},
         "RDNA3 native, validated on RX 7600 (gfx1102); the rest of the gfx110X "
         "family (RX 7700/7800/7900) is cross-compiled but untested on real hardware. "
         "On a mixed iGPU+dGPU system, vte-server (0.3.4+) enumerates every visible "
         "HIP device and auto-selects the first one matching a discrete RDNA2/RDNA3 "
         "architecture it actually supports, so an integrated GPU is never picked by "
         "accident. If a system exposes more than one discrete GPU VTE supports, it "
         "picks the first one found; there is no Lemonade-side way yet to request a "
         "specific one (see VTE_DEVICE_INDEX for a manual override on the vte-server "
         "side)."},
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
