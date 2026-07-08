#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace parakeet {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "parakeetcpp",
    /*display_name*/    "Parakeet.cpp",
#ifdef _WIN32
    /*binary*/          "parakeet-server.exe",
#else
    /*binary*/          "parakeet-server",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"parakeetcpp_backend", "--parakeetcpp", "", "BACKEND",
         "ParakeetCpp backend to use", "Parakeet.cpp Options"},
        {"parakeetcpp_args", "--parakeetcpp-args", "", "ARGS",
         "Custom arguments to pass to parakeet-server", "Parakeet.cpp Options"},
    },
    /*support*/ {
        {"cpu",    {"windows", "linux"},    {{"cpu", {"x86_64"}}},                   "x86_64 CPU"},
        {"vulkan", {"windows", "linux"},    {{"cpu", {"x86_64"}}, {"amd_gpu", {}}},  "x86_64 CPU / AMD GPU"},
        {"cuda",   {"windows", "linux"},    {{"nvidia_gpu", {}}},                    "NVIDIA GPU"},
        {"metal",  {"macos"},               {{"metal", {}}},                         "Apple Silicon GPU"},
    },
    /*default_labels*/  {"transcription"},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Speech-to-text",
    /*experimental*/    false,
    /*web_display_name*/ "parakeet.cpp",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace parakeet
}  // namespace backends
}  // namespace lemon
