#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace whispercpp {

// The whispercpp backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "whispercpp",
    /*display_name*/    "Whisper.cpp",
#ifdef _WIN32
    /*binary*/          "whisper-server.exe",
#else
    /*binary*/          "whisper-server",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_CPU,   // npu variant resolves to NPU + ExclusiveNpu via effective_*()
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"whispercpp_backend", "--whispercpp", "", "BACKEND",
         "WhisperCpp backend to use", "Whisper.cpp Options"},
        {"whispercpp_args", "--whispercpp-args", "", "ARGS",
         "Custom arguments to pass to whisper-server", "Whisper.cpp Options"},
    },
    /*support*/ {
        {"npu", {"windows"}, {{"amd_npu", {"XDNA2"}}}, "XDNA2 NPU"},
        {"rocm", {"windows", "linux"},
         {{"amd_gpu", {"gfx1150", "gfx1151", "gfx110X", "gfx120X"}}}, "Supported AMD ROCm iGPU/dGPU families*"},
        {"vulkan", {"windows", "linux"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}}, "x86_64 CPU"},
        {"cpu", {"windows", "linux"}, {{"cpu", {"x86_64"}}}, "x86_64 CPU"},
        {"metal", {"macos"}, {{"metal", {}}}, "Apple Silicon GPU"},
    },
    /*default_labels*/  {"transcription", "realtime-transcription"},
    /*required_checkpoints*/ {"main"},  // npu_cache validated in load() (npu variant only)
    /*modality*/        "Speech-to-text",
    /*experimental*/    false,
    /*web_display_name*/ "whisper.cpp",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {"cpu", "npu"},
    /*bin_variants*/    {"cpu", "npu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace whispercpp
}  // namespace backends
}  // namespace lemon
