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
    /*config_section*/  "moonshine",
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"moonshine_args", "--moonshine-args", "", "ARGS",
         "Custom arguments to pass to moonshine-server", ""},
    },
    /*support*/ {
        {"moonshine", "cpu", {"windows"}, {{"cpu", {"x86_64"}}}},
        {"moonshine", "cpu", {"linux"}, {{"cpu", {"x86_64", "arm64"}}}},
        {"moonshine", "cpu", {"macos"}, {{"cpu", {"arm64"}}}},
    },
    /*default_labels*/  {"transcription", "realtime-transcription"},
    /*required_checkpoints*/ {"main"},
};

}  // namespace moonshine
}  // namespace backends
}  // namespace lemon
