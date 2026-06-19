#include "lemon/backends/llamacpp_descriptor.h"

namespace lemon {
namespace backends {

const BackendDescriptor llamacpp_descriptor = {
    /*recipe*/          "llamacpp",
    /*display_name*/    "Llama.cpp GPU",
#ifdef _WIN32
    /*binary*/          "llama-server.exe",
#else
    /*binary*/          "llama-server",
#endif
    /*config_section*/  "llamacpp",
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
        {"llamacpp", "system", {"linux"}, {{"cpu", {"x86_64", "arm64"}}}},
        {"llamacpp", "metal", {"macos"}, {{"metal", {}}}},
        {"llamacpp", "cuda", {"windows", "linux"},
         {{"nvidia_gpu", {"sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120", "sm_121"}}}},
        {"llamacpp", "vulkan", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}, {"amd_gpu", {}}}},
        {"llamacpp", "rocm", {"windows", "linux"},
         {{"amd_gpu", {"gfx1150", "gfx1151", "gfx1152", "gfx103X", "gfx110X", "gfx120X"}}}},
        {"llamacpp", "cpu", {"windows", "linux"}, {{"cpu", {"x86_64", "arm64"}}}},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
};

} // namespace backends
} // namespace lemon
