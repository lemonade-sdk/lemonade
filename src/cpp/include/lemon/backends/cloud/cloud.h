#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace cloud {

// The cloud backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "cloud",
    /*display_name*/    "Cloud",
    /*binary*/          "",  // no subprocess: runs on a remote provider
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_NONE,
    /*slot_policy*/     SlotPolicy::Unmetered,  // never counts toward slots, never auto-evicted
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  true,   // models discovered at runtime from the provider
    /*options*/ {},
    /*support*/ {},             // no local gating: install/support machinery skips cloud
    /*default_labels*/  {},
    /*required_checkpoints*/ {},  // no downloaded files
    /*modality*/        "",
    /*experimental*/    false,
    /*web_display_name*/ "",
};

}  // namespace cloud
}  // namespace backends
}  // namespace lemon
