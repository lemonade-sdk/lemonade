#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace voxcpm2 {

// VoxCPM2 text-to-speech (OpenBMB), wrapped via llama.cpp-omni's resident
// llama-tts-server subprocess. Serves the /audio/speech capability.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "voxcpm2",
    /*display_name*/    "VoxCPM2",
#ifdef _WIN32
    /*binary*/          "llama-tts-server.exe",
#else
    /*binary*/          "llama-tts-server",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"voxcpm2_backend", "--voxcpm2", "", "BACKEND",
         "VoxCPM2 backend to use", "Text-to-Speech Options"},
    },
    /*support*/ {
        {"metal", {"macos"}, {{"metal", {}}}, "Apple Silicon GPU"},
        {"cuda", {"linux", "windows"}, {{"nvidia_gpu", {}}}, "NVIDIA GPUs"},
        {"vulkan", {"linux", "windows"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}, {"nvidia_gpu", {}}}, "Vulkan-capable GPUs"},
        {"cpu", {"linux", "windows"}, {{"cpu", {"x86_64"}}}, "x86_64 CPU"},
    },
    /*default_labels*/  {"tts"},
    /*required_checkpoints*/ {"main", "acoustic"},  // BaseLM + Acoustic GGUF, both always needed
    /*modality*/        "Text-to-speech",
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"vulkan", "cuda", "cpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace voxcpm2
}  // namespace backends
}  // namespace lemon
