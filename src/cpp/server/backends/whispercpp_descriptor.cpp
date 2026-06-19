#include "lemon/backends/whispercpp_descriptor.h"

namespace lemon {
namespace backends {

const BackendDescriptor whispercpp_descriptor = {
    /*recipe*/          "whispercpp",
    /*display_name*/    "Whisper.cpp",
#ifdef _WIN32
    /*binary*/          "whisper-server.exe",
#else
    /*binary*/          "whisper-server",
#endif
    /*config_section*/  "whispercpp",
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
        {"whispercpp", "npu", {"windows"}, {{"amd_npu", {"XDNA2"}}}},
        {"whispercpp", "rocm", {"windows", "linux"},
         {{"amd_gpu", {"gfx1150", "gfx1151", "gfx110X", "gfx120X"}}}},
        {"whispercpp", "vulkan", {"windows", "linux"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}}},
        {"whispercpp", "cpu", {"windows", "linux"}, {{"cpu", {"x86_64"}}}},
        {"whispercpp", "metal", {"macos"}, {{"metal", {}}}},
    },
    /*default_labels*/  {"transcription", "realtime-transcription"},
    /*required_checkpoints*/ {"main"},  // npu_cache validated in load() (npu variant only)
};

} // namespace backends
} // namespace lemon
