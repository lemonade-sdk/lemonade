#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace ds4 {

// The ds4 backend descriptor (plain data). DS4 (DwarfStar) is antirez's
// self-contained DeepSeek V4 inference engine with an OpenAI-compatible HTTP
// server (ds4-server). Upstream publishes no binaries, releases or tags, so
// builds come from lemonade-sdk/ds4-rocm, which compiles a pinned upstream
// commit and bundles the ROCm runtime alongside it.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ds4",
    /*display_name*/    "DS4 DwarfStar (experimental)",
    /*binary*/          "ds4-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"ds4_args", "--ds4-args", "", "ARGS",
         "Custom arguments to pass to ds4-server", "DS4 Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "Prebuilt ds4 for AMD Strix Halo"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},  // single rocm artifact, no stable/nightly channels
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace ds4
}  // namespace backends
}  // namespace lemon
