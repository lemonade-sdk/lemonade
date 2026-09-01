#pragma once

#include "lemon/backends/backend_descriptor.h"
#include "lemon/kv_cache_quant.h"

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

// (Backend, tier) fused-attention-kernel safety table for R5/KTD6. Lemonade
// installs prebuilt llama.cpp binaries rather than compiling them, so this
// table cannot be derived from this repo's own build flags — each row is
// hand-verified against the actual asset source for that backend and cites
// where. A backend absent from this table (or an unrecognized one) defaults
// to `false, false` via KvCacheQuantSafetyTable's lookup miss, never `true`.
//
// Restricted to q8_0 and q4_0 deliberately (KTD6): those are the two tiers
// whose symmetric fused-attention kernels compile unconditionally in a stock
// llama.cpp build, so a `true` entry needs no build-flag knowledge beyond
// "this build was not stripped of kernel instances." The wider llama.cpp
// quant set (q4_1, q5_0, q5_1) needs a non-default build flag
// (GGML_CUDA_FA_ALL_QUANTS) and is deferred rather than admitted.
inline const KvCacheQuantSafetyTable kv_cache_quant_safety_table = {
    // cuda / rocm-stable: lemonade-sdk/llama.cpp (a thin release wrapper with
    // no vendored source — its .github/workflows/release.yml clones
    // ggml-org/llama.cpp fresh per run). Verified against that workflow's
    // CMAKE_ARGS for both the CUDA and ROCm jobs: neither passes
    // GGML_CUDA_FA_ALL_QUANTS (or a HIP equivalent), so the build stays at
    // upstream's default instance set, which compiles the symmetric q8_0 and
    // q4_0 vec-FA kernels unconditionally
    // (ggml/src/ggml-cuda/CMakeLists.txt, ggml/src/ggml-hip/CMakeLists.txt).
    {"cuda",         {true, true}},
    {"rocm-stable",  {true, true}},
    // rocm-nightly: lemonade-sdk/llamacpp-rocm. Also source-less — its
    // .github/workflows/build-llamacpp-rocm.yml clones
    // https://github.com/ggerganov/llama.cpp.git at a pinned tag or master.
    // Its CMake invocation passes GGML_HIP_ROCWMMA_FATTN=OFF (rocWMMA kernel
    // selection, unrelated to quant-type instantiation) and no FA-all-quants
    // equivalent, so the same unconditional symmetric q8_0/q4_0 instances
    // apply.
    {"rocm-nightly", {true, true}},
    // metal: ggml-org/llama.cpp, upstream default build. Verified directly:
    // ggml/src/ggml-metal/ggml-metal.metal instantiates
    // kernel_flash_attn_ext_q8_0_* and kernel_flash_attn_ext_q4_0_* (and the
    // _vec_ variants) unconditionally, with no build flag gating them.
    {"metal",        {true, true}},
    // vulkan: flash attention is gated on per-device GPU feature bits
    // (coopmat2 or subgroupShuffle+subgroupVote), not on the shipped binary,
    // so no static table entry can assert it — a rejected op silently falls
    // back to CPU attention. Enabling Vulkan is out of scope for this
    // change (see plan Risks & Dependencies / R14).
    {"vulkan",       {false, false}},
    // cpu: no fused flash-attention kernels at all.
    {"cpu",          {false, false}},
    // system: a pre-installed binary from PATH. Build provenance and flags
    // are unknown, so it ships unsafe rather than guessed.
    {"system",       {false, false}},
};

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
