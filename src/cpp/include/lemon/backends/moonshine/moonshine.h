#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace moonshine {

// The moonshine backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "moonshine",
    /*display_name*/    "Moonshine",
    /*binary*/          "moonshine-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"moonshine_args", "--moonshine-args", "", "ARGS",
         "Custom arguments to pass to moonshine-server", "Moonshine Options"},
    },
    /*support*/ {
        {"cpu", {"windows"}, {{"cpu", {"x86_64"}}}, "x86_64/arm64 CPU"},
        {"cpu", {"linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64/arm64 CPU"},
        {"cpu", {"macos"}, {{"cpu", {"arm64"}}}, "x86_64/arm64 CPU"},
    },
    /*default_labels*/  {"transcription", "realtime-transcription"},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Speech-to-text",
    /*experimental*/    false,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {"cpu"},
    /*bin_variants*/    {"cpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace moonshine
}  // namespace backends
}  // namespace lemon
