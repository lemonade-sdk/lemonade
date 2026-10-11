#pragma once

#include "lemon/auto_tune.h"
#include "lemon/backends/backend_descriptor.h"
#include "lemon/backends/backend_utils.h"
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
        {"kv_cache_quantization", "--kv-cache-quantization", "auto", "ARGS",
         "KV cache quantization: auto (default), off, q8_0, or q4_0", "Llama.cpp Backend Options"},
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
        {"kv_cache_quantization", "auto"},
    },
};

// (Backend, tier) fused-attention-kernel safety table. Lemonade
// installs prebuilt llama.cpp binaries rather than compiling them, so this
// table cannot be derived from this repo's own build flags — each row is
// hand-verified against the actual asset source for that backend and cites
// where. A backend absent from this table (or an unrecognized one) defaults
// to `false, false` via KvCacheQuantSafetyTable's lookup miss, never `true`.
//
// Restricted to q8_0 and q4_0 deliberately: those are the two tiers
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
    // back to CPU attention. Enabling Vulkan here would need a per-device
    // GPU feature-bit query this static table format doesn't carry.
    {"vulkan",       {false, false}},
    // cpu: no fused flash-attention kernels at all.
    {"cpu",          {false, false}},
    // system: a pre-installed binary from PATH. Build provenance and flags
    // are unknown, so it ships unsafe rather than guessed.
    {"system",       {false, false}},
};



// The --cache-type-k/--cache-type-v launch fragment for a resolved KV cache
// tier — no memory reasoning or conflict detection here.
// Empty for f16 or an unresolved tier: the whole point is that the
// managed flags are reserved only when actually emitted, so an unconditional
// `llamacpp_args --cache-type-k ...` workaround keeps working exactly as it
// does today while the option stays f16.
struct CacheTypeLaunchArgs {
    std::vector<std::string> argv;  // tokens to append, e.g. {"--cache-type-k","q8_0","--cache-type-v","q8_0"}
    // {flag, short alias} pairs to reserve, e.g. {"--cache-type-k","-ctk"}, {"--cache-type-v","-ctv"}.
    // Each flag reserves its own alias independently, matching --device/-dev.
    std::vector<std::pair<std::string, std::string>> reservations;
};

inline CacheTypeLaunchArgs kv_cache_type_launch_args(std::optional<KvCacheQuantTier> resolved_tier) {
    CacheTypeLaunchArgs out;
    if (!resolved_tier || *resolved_tier == KvCacheQuantTier::F16) {
        return out;
    }
    const std::string tier_str = kv_cache_quant_tier_to_string(*resolved_tier);
    out.argv = {"--cache-type-k", tier_str, "--cache-type-v", tier_str};
    out.reservations = {{"--cache-type-k", "-ctk"}, {"--cache-type-v", "-ctv"}};
    return out;
}

// Describes a manual llamacpp_args passthrough of --cache-type-k/-v (or the
// -ctk/-ctv aliases) that collides with the flags a resolved tier would
// manage — the pre-existing workaround for setting them before this feature
// existed. Pure and testable with a fabricated tier/args pair, independent of
// the live-memory-dependent resolve_llamacpp_kv_cache below. Returns an empty
// string when there is no collision.
inline std::string kv_cache_type_flag_collision(std::optional<KvCacheQuantTier> resolved_tier,
                                                 const std::string& llamacpp_args) {
    const CacheTypeLaunchArgs cache_type_args = kv_cache_type_launch_args(resolved_tier);
    if (cache_type_args.reservations.empty() || llamacpp_args.empty()) return "";
    // Not validate_custom_args: its message says the flag is managed by
    // Lemonade and cannot be overridden, which is the opposite of what
    // happens here — the user's flag wins and tier management stands down.
    const auto args_map = utils::build_custom_args_map(utils::parse_custom_args(llamacpp_args));
    std::string flags;
    for (const auto& [flag, alias] : cache_type_args.reservations) {
        for (const std::string& spelling : {flag, alias}) {
            if (args_map.find(spelling) == args_map.end()) continue;
            if (!flags.empty()) flags += ", ";
            flags += spelling;
        }
    }
    if (flags.empty()) return "";
    return "llamacpp_args sets the KV cache type itself (" + flags +
        "), so kv_cache_quantization is not applied and the manual flags are used as-is.";
}

// Both the load path (Router::load_model) and the options-preview path
// (Server::respond_with_model_options) need the same three steps before
// they can act on a KV cache resolution: normalize this model's backend
// choice, query live available memory, and resolve the tier/ctx_size
// against llamacpp's safety table. Bundled here (rather than in auto_tune.h)
// because it names `kv_cache_quant_safety_table`, keeping the pure resolver
// free of any specific backend's table (see resolve_kv_cache's doc comment).
struct LlamaCppKvCacheContext {
    std::string normalized_backend;
    double available_memory_gb = 0.0;
    KvCacheResolution resolution;

    // Non-empty when the user's own llamacpp_args already set the KV cache
    // type, which disables tier management for this load rather than failing it.
    std::string warning;
};

inline LlamaCppKvCacheContext resolve_llamacpp_kv_cache(const RecipeOptions& effective_options,
                                                         const ModelInfo& model_info) {
    LlamaCppKvCacheContext ctx;
    const json backend_choice_json = effective_options.get_option(model_info.recipe + "_backend");
    ctx.normalized_backend = backends::normalize_backend_name(
        model_info.recipe,
        backend_choice_json.is_string() ? backend_choice_json.get<std::string>() : "");
    // An explicit ctx_size with quantization off never reaches a branch that
    // consults memory (resolve_kv_cache_f16_only's explicit-ctx_size path
    // returns before reading it), so skip the live device-memory query (an
    // nvidia-smi subprocess spawn on NVIDIA) for that case.
    // Excludes NPU: Router::load_model reuses this same available_memory_gb
    // for its own low-memory rejection check, which needs the real value
    // regardless of what the kv-cache resolver itself would consult.
    const json ctx_size_json = effective_options.get_option("ctx_size");
    const bool ctx_size_explicit = ctx_size_json.is_number() && ctx_size_json.get<int64_t>() != -1;
    const json kv_quant_json = effective_options.get_option("kv_cache_quantization");
    const bool kv_quant_is_off = kv_quant_json.is_string() && kv_quant_json.get<std::string>() == "off";
    const bool skip_memory_probe = ctx_size_explicit && kv_quant_is_off &&
        !(model_info.device & DEVICE_NPU);
    const json device_choice_json = effective_options.get_option(model_info.recipe + "_device");
    const std::string device_choice =
        device_choice_json.is_string() ? device_choice_json.get<std::string>() : "";
    ctx.available_memory_gb = skip_memory_probe
        ? 0.0
        : get_available_memory_gb(
              model_info.device,
              gpu_memory_vendor_for_target(ctx.normalized_backend, device_choice), device_choice);
    ctx.resolution = resolve_kv_cache(effective_options, model_info, ctx.available_memory_gb,
                                      ctx.normalized_backend, kv_cache_quant_safety_table);
    if (!ctx.resolution.ok()) return ctx;

    const json llamacpp_args_json = effective_options.get_option("llamacpp_args");
    ctx.warning = kv_cache_type_flag_collision(
        ctx.resolution.tier,
        llamacpp_args_json.is_string() ? llamacpp_args_json.get<std::string>() : "");
    if (!ctx.warning.empty()) {
        // The user's own flags win. Re-resolve with quantization off so the
        // context size is sized for an unquantized cache: the managed flags
        // are not emitted, and sizing context for a tier that will not be in
        // effect would over-reserve and push the backend into an OOM.
        RecipeOptions unmanaged_options = effective_options;
        unmanaged_options.set_option("kv_cache_quantization", "off");
        ctx.resolution = resolve_kv_cache(unmanaged_options, model_info, ctx.available_memory_gb,
                                          ctx.normalized_backend, kv_cache_quant_safety_table);
    }
    return ctx;
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
