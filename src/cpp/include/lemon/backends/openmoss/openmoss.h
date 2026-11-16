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
        {"pcm_sample_rate", "", 0, "SIZE", "Sample rate of the PCM this model emits, reported as X-MOSS-Sample-Rate (0 omits the header)", "Text-to-Speech Options", false},
        {"pcm_channels", "", 0, "SIZE", "Channel count of the PCM this model emits, reported as X-MOSS-Channels (0 omits the header)", "Text-to-Speech Options", false},
    },
    /*support*/ {
        {"cuda", BackendTier::Experimental, BackendFormat::Native, {"linux", "windows"}, {{"nvidia_gpu", {}}}, "NVIDIA GPUs"},
        {"vulkan", BackendTier::Experimental, BackendFormat::Native, {"linux", "windows"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}, {"nvidia_gpu", {}}}, "Vulkan-capable GPUs"},
    },
    /*about*/ {
        {"cuda", {"MOSS speech and sound-effect models in C++/GGML on top of llama.cpp, on NVIDIA GPUs.",
                  "Piotr Wilkin (pwilkin)", "Piotr Wilkin (pwilkin)", "Apache-2.0", "https://github.com/pwilkin/openmoss"}},
        {"vulkan", {"MOSS speech and sound-effect models in C++/GGML on top of llama.cpp, on Vulkan-capable GPUs.",
                    "Piotr Wilkin (pwilkin)", "Piotr Wilkin (pwilkin)", "Apache-2.0", "https://github.com/pwilkin/openmoss"}},
    },
    /*supported_modes*/ {"tts", "audio-generation"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"vulkan", "cuda"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace openmoss
}  // namespace backends
}  // namespace lemon
