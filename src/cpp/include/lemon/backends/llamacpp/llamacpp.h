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
        {"kv_cache_quantization", "--kv-cache-quantization", "f16", "ARGS",
         "KV cache quantization: f16 (default), auto, q8_0, or q4_0", "Llama.cpp Backend Options"},
        {"max_kv_quantization", "--max-kv-quantization", "f16", "ARGS",
         "Highest-quality KV cache tier the auto ladder may select", "Llama.cpp Backend Options"},
        {"min_kv_quantization", "--min-kv-quantization", "q8_0", "ARGS",
         "Lowest-quality KV cache tier the auto ladder may select", "Llama.cpp Backend Options"},
        {"kv_cache_priority", "--kv-cache-priority", "balanced", "ARGS",
         "KV cache ladder bias: max_context, balanced (default), or max_speed", "Llama.cpp Backend Options"},
    },
    /*support*/ {
        {"system", {"linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64/ARM64 CPU, GPU"},
        {"metal", {"macos"}, {{"metal", {}}}, "Apple Silicon GPU"},
        {"cuda", {"windows", "linux"},
         {{"nvidia_gpu", {"sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120", "sm_121"}}}, "NVIDIA GPUs (Turing or newer)**"},
        {"vulkan", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}, {"amd_gpu", {}}}, "x86_64 CPU, AMD iGPU, AMD dGPU; ARM64 CPU/GPU (Linux)"},
        {"rocm", {"windows", "linux"},
         {{"amd_gpu", {"gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X", "gfx908", "gfx90a", "gfx942", "gfx950"}}}, "AMD GPUs supported by ROCm",
         // CDNA arches (gfx908, gfx90a, gfx942) lack Windows ROCm binaries; gfx950
         // additionally lacks a nightly-channel asset.
         {{"gfx950", {/*os*/ {"linux"}, /*channels*/ {"stable"}}},
          {"gfx908", {/*os*/ {"linux"}, /*channels*/ {}}},
          {"gfx90a", {/*os*/ {"linux"}, /*channels*/ {}}},
          {"gfx942", {/*os*/ {"linux"}, /*channels*/ {}}}}},
        {"cpu", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64 CPU; ARM64 CPU (Linux)"},
    },
    /*supported_modes*/ {"chat", "embeddings", "reranking"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
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
    /*config_extra*/    {
        {"prefer_system", true},
        {"kv_cache_quantization", "f16"},
        {"max_kv_quantization", "f16"},
        {"min_kv_quantization", "q8_0"},
        {"kv_cache_priority", "balanced"},
    },
};

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
