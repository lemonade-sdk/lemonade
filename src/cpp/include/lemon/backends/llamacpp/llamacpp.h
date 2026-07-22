#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace llamacpp {

// The llamacpp backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "llamacpp",
    /*display_name*/    "Llama.cpp GPU",
#ifdef _WIN32
    /*binary*/          "llama-server.exe",
#else
    /*binary*/          "llama-server",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,   // cpu/system variants resolve to CPU via effective_device()
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"llamacpp_backend", "--llamacpp", "", "BACKEND",
         "LlamaCpp backend to use", "Llama.cpp Backend Options"},
        {"llamacpp_device", "--llamacpp-device", "", "DEVICES",
         "Comma-separated list of accelerator devices to use (e.g. Vulkan0)", "Llama.cpp Backend Options"},
        {"llamacpp_args", "--llamacpp-args", "", "ARGS",
         "Custom arguments to pass to llama-server", "Llama.cpp Backend Options"},
    },
    /*support*/ {
        {"system", {"linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64/ARM64 CPU, GPU"},
        {"metal", {"macos"}, {{"metal", {}}}, "Apple Silicon GPU"},
        {"cuda", {"windows", "linux"},
         {{"nvidia_gpu", {"sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120", "sm_121"}}}, "NVIDIA GPUs (Turing or newer)**"},
        {"vulkan", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}, {"amd_gpu", {}}}, "x86_64 CPU, AMD iGPU, AMD dGPU; ARM64 CPU/GPU (Linux)"},
        {"rocm", {"windows", "linux"},
         {{"amd_gpu", {"gfx1150", "gfx1151", "gfx1152", "gfx103X", "gfx110X", "gfx120X", "gfx942", "gfx950"}}}, "Supported AMD ROCm iGPU/dGPU families, incl. AMD Instinct MI300X (gfx942) and MI350X (gfx950, Linux + stable only)*",
         // gfx950 (MI350) only ships a Linux + stable-channel asset so far; the
         // Windows TheRock dist and the nightly build aren't published yet.
         {{"gfx950", {/*os*/ {"linux"}, /*channels*/ {"stable"}}}}},
        {"cpu", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64 CPU; ARM64 CPU (Linux)"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    false,
    /*web_display_name*/ "llama.cpp GPU",
    /*rocm_channels*/   {"stable", "nightly"},
    /*exposes_prometheus_metrics*/ true,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {"rocm", "vulkan", "cpu"},
    /*bin_variants*/    {"rocm", "vulkan", "cuda", "cpu"},
    /*config_extra*/    {{"prefer_system", true}},
};

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
