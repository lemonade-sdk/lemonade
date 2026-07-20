#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace sdcpp {

// The sdcpp backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "sd-cpp",
    /*display_name*/    "StableDiffusion.cpp",
#ifdef _WIN32
    /*binary*/          "sd-server.exe",
#else
    /*binary*/          "sd-server",
#endif
    /*config_section*/  "sdcpp",
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"sd-cpp_backend", "--sdcpp", "", "BACKEND",
         "SD.cpp backend to use", "Stable Diffusion Options"},
        {"sdcpp_args", "--sdcpp-args", "", "ARGS",
         "Custom arguments to pass to sd-server (must not conflict with managed args)", "Stable Diffusion Options"},
        // Image generation defaults (recipe-level only, not CLI flags).
        {"steps", "", 20, "SIZE", "Number of diffusion steps", "Stable Diffusion Options"},
        {"cfg_scale", "", 7.0, "SIZE", "Classifier-free guidance scale", "Stable Diffusion Options"},
        {"width", "", 512, "SIZE", "Output image width", "Stable Diffusion Options"},
        {"height", "", 512, "SIZE", "Output image height", "Stable Diffusion Options"},
        {"sampling_method", "", "", "ARGS", "Sampling method", "Stable Diffusion Options"},
        {"flow_shift", "", 0.0, "SIZE", "Flow shift", "Stable Diffusion Options"},
    },
    /*support*/ {
        {"rocm", {"windows", "linux"},
         {{"amd_gpu", {"gfx1150", "gfx1151", "gfx1152", "gfx103X", "gfx110X", "gfx120X"}}}, "Supported AMD ROCm iGPU/dGPU families*"},
        {"cuda", {"windows", "linux"},
         {{"nvidia_gpu", {"sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120", "sm_121"}}}, "NVIDIA GPUs (Turing or newer)**"},
        {"vulkan", {"windows", "linux"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}, {"nvidia_gpu", {}}}, "Vulkan-capable GPUs"},
        {"cpu", {"windows", "linux"}, {{"cpu", {"x86_64"}}}, "x86_64 CPU"},
        {"metal", {"macos"}, {{"metal", {}}}, "Apple Silicon GPU"},
    },
    /*default_labels*/  {"image"},
    /*required_checkpoints*/ {"main"},  // flux text_encoder+vae validated together in load()
    /*modality*/        "Image generation",
    /*experimental*/    false,
    /*web_display_name*/ "stable-diffusion.cpp",
    /*rocm_channels*/   {"stable"},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {"cpu", "rocm", "vulkan", "cuda"},
    /*bin_variants*/    {"cpu", "rocm", "vulkan", "cuda"},
    /*config_extra*/    {{"steps", 20}, {"cfg_scale", 7.0}, {"width", 512}, {"height", 512}},
};

}  // namespace sdcpp
}  // namespace backends
}  // namespace lemon
