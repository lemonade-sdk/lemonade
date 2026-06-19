#include "lemon/backends/kokoro_descriptor.h"

namespace lemon {
namespace backends {

const BackendDescriptor kokoro_descriptor = {
    /*recipe*/          "kokoro",
    /*display_name*/    "Kokoro",
#ifdef _WIN32
    /*binary*/          "koko.exe",
#else
    /*binary*/          "koko",
#endif
    /*config_section*/  "kokoro",
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"kokoro", "cpu", {"windows", "linux"}, {{"cpu", {"x86_64"}}}},
        {"kokoro", "metal", {"macos"}, {{"metal", {}}}},
    },
    /*default_labels*/  {},  // kokoro models carry "tts" explicitly in server_models.json
    /*required_checkpoints*/ {"main"},
};

} // namespace backends
} // namespace lemon
