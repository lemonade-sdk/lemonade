#include "lemon/backends/fastflowlm_descriptor.h"

namespace lemon {
namespace backends {

const BackendDescriptor fastflowlm_descriptor = {
    /*recipe*/          "flm",
    /*display_name*/    "FastFlowLM NPU",
#ifdef _WIN32
    /*binary*/          "flm.exe",
#else
    /*binary*/          "flm",
#endif
    /*config_section*/  "flm",
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::CoexistByType,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"flm", "npu", {"windows", "linux"}, {{"amd_npu", {"XDNA2"}}}},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
};

} // namespace backends
} // namespace lemon
