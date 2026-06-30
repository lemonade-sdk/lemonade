#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace acestep {

// ACE-Step music generation, wrapped via its resident ace-server (an async job
// API). Serves the /audio/generations capability.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "acestep",
    /*display_name*/    "ACE-Step",
    /*binary*/          "ace-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"vulkan", {"linux", "windows"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}, {"nvidia_gpu", {}}}, "Vulkan-capable GPUs"},
        {"rocm", {"linux", "windows"}, {{"amd_gpu", {}}}, "AMD GPUs (ROCm via TheRock)"},
    },
    /*default_labels*/  {"audio-generation"},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Audio generation",
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {"stable"},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"vulkan", "rocm"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace acestep
}  // namespace backends
}  // namespace lemon
