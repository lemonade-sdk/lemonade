#include "lemon/backends/cloud_descriptor.h"

namespace lemon {
namespace backends {

const BackendDescriptor cloud_descriptor = {
    /*recipe*/          "cloud",
    /*display_name*/    "Cloud",
    /*binary*/          "",  // no subprocess: runs on a remote provider
    /*config_section*/  "cloud",
    /*default_device*/  DEVICE_NONE,
    /*slot_policy*/     SlotPolicy::Unmetered,  // never counts toward slots, never auto-evicted
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  true,   // models discovered at runtime from the provider
    /*options*/ {},
    /*support*/ {},             // no local gating: install/support machinery skips cloud
    /*default_labels*/  {},
    /*required_checkpoints*/ {},  // no downloaded files
};

} // namespace backends
} // namespace lemon
