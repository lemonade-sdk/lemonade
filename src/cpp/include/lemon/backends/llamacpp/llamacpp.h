#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace llamacpp {

// Flags Lemonade owns on a containerized llama-server. A later flag wins in
// llama.cpp's left-to-right parse, so a user-supplied copy of any of these would
// move the port out from under the proxy, rebind the host, or swap the model.
// The binary path builds its reserved set as it assembles argv; a container
// launch takes a fixed command, so it states the set instead.
inline const std::set<std::string>& reserved_custom_arg_flags() {
    static const std::set<std::string> kReserved = {
        "-m", "--model",
        "-mu", "--model-url",
        "-hf", "--hf-repo", "--hf-file",
        "--host", "--port",
        "-c", "--ctx-size",
        "--jinja", "--no-jinja",
        "--metrics",
        "--mmproj", "--no-mmproj",
    };
    return kReserved;
}

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
         {{"amd_gpu", {"gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X", "gfx908", "gfx90a", "gfx942", "gfx950"}}}, "AMD GPUs supported by ROCm",
         // CDNA arches (gfx908, gfx90a, gfx942) lack Windows ROCm binaries; gfx950
         // additionally lacks a nightly-channel asset.
         {{"gfx950", {/*os*/ {"linux"}, /*channels*/ {"stable"}}},
          {"gfx908", {/*os*/ {"linux"}, /*channels*/ {}}},
          {"gfx90a", {/*os*/ {"linux"}, /*channels*/ {}}},
          {"gfx942", {/*os*/ {"linux"}, /*channels*/ {}}}}},
        {"cpu", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64 CPU; ARM64 CPU (Linux)"},
        // Nathan W's Vulkan performance fork. It reads ordinary GGUFs, so it is
        // a build of this recipe rather than an engine of its own; it just
        // happens to ship as a container instead of a release asset, which
        // backend_versions.json records by pinning an image for it. Opt-in only:
        // never_default keeps a fresh install on the stock Vulkan build.
        {"nathanw", {"linux"}, {{"amd_gpu", {"gfx1151"}}},
         "AMD Strix Halo (Nathan W's Vulkan performance fork)**", {}, /*never_default*/ true},
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
    /*config_extra*/    {{"prefer_system", true}},
};

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
