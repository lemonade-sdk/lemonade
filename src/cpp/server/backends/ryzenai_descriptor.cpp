#include "lemon/backends/ryzenai_descriptor.h"

namespace lemon {
namespace backends {

const BackendDescriptor ryzenai_descriptor = {
    /*recipe*/          "ryzenai-llm",
    /*display_name*/    "Ryzen AI LLM",
#ifdef _WIN32
    /*binary*/          "ryzenai-server.exe",
#else
    /*binary*/          "ryzenai-server",
#endif
    /*config_section*/  "ryzenai",
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"ryzenai-llm", "npu", {"windows"}, {{"amd_npu", {"XDNA2"}}}},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
};

} // namespace backends
} // namespace lemon
