#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace openmoss {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "openmoss",
    /*display_name*/    "OpenMOSS TTS",
    /*binary*/          "moss-tts-server",
    /*config_section*/  "",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"openmoss_backend", "--openmoss", "", "BACKEND",
         "OpenMOSS TTS backend to use", "Text-to-Speech Options"},
    },
    /*support*/ {
        {"rocm", {"linux", "windows"}, {{"amd_gpu", {}}}, "AMD GPUs (ROCm via TheRock)"},
        {"cuda", {"linux", "windows"}, {{"nvidia_gpu", {}}}, "NVIDIA GPUs"},
        {"vulkan", {"linux", "windows"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}, {"nvidia_gpu", {}}}, "Vulkan-capable GPUs"},
    },
    /*default_labels*/  {"tts"},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text-to-speech",
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {"stable"},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"vulkan", "rocm", "cuda"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace openmoss
}  // namespace backends
}  // namespace lemon
