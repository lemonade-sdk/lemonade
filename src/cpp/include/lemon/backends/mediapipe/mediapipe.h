#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace mediapipe {

// The mediapipe backend descriptor (plain data). Runs Google MediaPipe task
// models (hand landmarks) on the CPU from a Python venv that lemonade builds
// with the pinned mediapipe wheel on first install, or from a user-supplied
// Python (mediapipe.cpu_bin) that already has it.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "mediapipe",
    /*display_name*/    "MediaPipe",
#ifdef _WIN32
    /*binary*/          "python.exe",
#else
    /*binary*/          "python",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"mediapipe_args", "--mediapipe-args", "", "ARGS",
         "Custom arguments to pass to mediapipe_server.py", "MediaPipe Options"},
    },
    /*support*/ {
        {"cpu", BackendTier::Experimental, BackendFormat::Python, {"windows"},
         {{"cpu", {"x86_64", "arm64"}}}, "x86_64/arm64 CPU"},
        {"cpu", BackendTier::Experimental, BackendFormat::Python, {"linux"},
         {{"cpu", {"x86_64", "arm64"}}}, "x86_64/arm64 CPU"},
        {"cpu", BackendTier::Experimental, BackendFormat::Python, {"macos"},
         {{"cpu", {"arm64"}}}, "arm64 CPU"},
    },
    /*supported_modes*/ {"hands"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {"cpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace mediapipe
}  // namespace backends
}  // namespace lemon
