#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace fastflowlm {

// The fastflowlm backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "flm",
    /*display_name*/    "FastFlowLM NPU",
#ifdef _WIN32
    /*binary*/          "flm.exe",
#else
    /*binary*/          "flm",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::CoexistByType,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  true,  // models come from flm's model_list.json, not server_models.json
    /*options*/ {
        {"flm_args", "--flm-args", "", "ARGS",
         "Safe flm serve tuning args: --pmode, --prefill-chunk-len, "
         "--img-pre-resize, --socket, --q-len, --preemption",
         "FastFlowLM Options"},
    },
    /*support*/ {
        {"npu", {"windows", "linux"}, {{"amd_npu", {"XDNA2"}}}, "XDNA2 NPU"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    false,
    /*web_display_name*/ "FastFlowLM NPU",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::AtLeast,  // system-managed package
    /*self_manages_downloads*/ true,  // flm pulls its own models via the flm CLI
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    {{"prefer_system", false}},
};

}  // namespace fastflowlm
}  // namespace backends
}  // namespace lemon
