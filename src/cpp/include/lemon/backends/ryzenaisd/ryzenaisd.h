#pragma once
#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace ryzenaisd {

// The ryzenaisd backend descriptor (plain data). Header-only `inline const` so
// it links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ryzenai-sd",
    /*display_name*/    "Ryzen AI SD",
#ifdef _WIN32
    /*binary*/          "ryzenai-sd-server.exe",
#else
    /*binary*/          "ryzenai-sd-server",
#endif
    /*config_section*/  "ryzenaisd",
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"ryzenaisd_args", "--ryzenaisd-args", "", "ARGS",
         "Custom arguments to pass to ryzenai-sd-server (must not conflict with managed args)",
         "Ryzen AI SD Options"},
        {"steps",  "", 20,  "SIZE", "Number of diffusion steps",  "Ryzen AI SD Options"},
        {"cfg_scale", "", 7.0, "SIZE", "Classifier-free guidance scale", "Ryzen AI SD Options"},
        {"width",  "", 512, "SIZE", "Output image width",         "Ryzen AI SD Options"},
        {"height", "", 512, "SIZE", "Output image height",        "Ryzen AI SD Options"},
    },
    /*support*/ {
        // Windows-only: AMD NPU (XDNA / Strix)
        {"npu", {"windows"}, {{"amd_npu", {}}}, "AMD Ryzen AI NPU (XDNA)"},
    },
    /*default_labels*/  {"image"},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Image generation",
    /*experimental*/    true,
    /*web_display_name*/ "ryzenai-sd",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"npu"},
    /*config_extra*/    {{"steps", 20}, {"cfg_scale", 7.0}, {"width", 512}, {"height", 512}},
};

}  // namespace ryzenaisd
}  // namespace backends
}  // namespace lemon
