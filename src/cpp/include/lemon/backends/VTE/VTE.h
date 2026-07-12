#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace VTE {

// The VTE backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
//
// NOTE: the folder/namespace is deliberately "VTE" (uppercase), unlike every
// other backend's lowercase stem (llamacpp, vllm, moonshine) -- this is an
// intentional deviation to keep the VTE integration visually distinct inside
// this repository. The `recipe` string below stays lowercase ("vte"),
// consistent with every other recipe identifier used in JSON/CLI.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "vte",
    /*display_name*/    "VTE (RDNA3 native, experimental)",
    /*binary*/          "vte-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // RDNA3-HIP-only: no CPU/Vulkan/CUDA flavor to select between
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},  // no backend-specific knobs for v1
    /*support*/ {
        {"rocm", {"windows"}, {{"amd_gpu", {"gfx110X"}}}, "RDNA3 native (RX 7700/7800/7900 series)"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    true,  // opt-in by default: VTE must not compete for VRAM with a
                                // backend the user didn't explicitly choose to load
    /*web_display_name*/ "",
    /*rocm_channels*/   {},  // single artifact, no stable/nightly channels
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,  // VTE talks to amdhip64.dll directly, no Triton/ROCm-PyTorch
                                        // kernel path -- not yet confirmed this is unaffected by the
                                        // same class of issue vLLM/gfx1151 hits; revisit if a real
                                        // report surfaces (see the project's own "measure, don't
                                        // guess" discipline -- this is a starting assumption, not a
                                        // verified one).
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"rocm"},  // mirrors Moonshine's single-flavor {"cpu"} pattern:
                                    // selectable_backend=false, but install/binary-path lookup
                                    // still uses an internal flavor string ("rocm").
    // NOTE: do NOT put a `ctx_size` default here. `config_extra` values are
    // written into config.json's per-recipe section (looks correct on disk),
    // but `RuntimeConfig::recipe_options()` (runtime_config.cpp) only ever
    // translates options a descriptor explicitly declares in `options` --
    // `ctx_size` is the one shared option opted into via `uses_ctx_size`
    // instead, and that function's special-cased ctx_size handling only reads
    // the GLOBAL top-level key, never a per-recipe one. Confirmed by testing
    // (not assumed): setting `config_extra: {{"ctx_size", 8192}}` here left
    // the persisted config.json looking right but had zero effect on the
    // actual loaded context size. The real per-model default lives in
    // server_models.json's `recipe_options` instead (see that file) -- this
    // is a genuine gap in Lemonade's own per-recipe ctx_size support, not
    // specific to VTE; fixing it properly needs `recipe_options()` to accept
    // the recipe name and check `uses_ctx_size` descriptors' own sections,
    // which is a wider change than this integration's scope.
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace VTE
}  // namespace backends
}  // namespace lemon
