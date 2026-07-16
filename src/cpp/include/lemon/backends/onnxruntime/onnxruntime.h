#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace onnxruntime {

// The ONNX Runtime backend descriptor (plain data). A generic ORT model server
// (see lemonade-sdk/ort-server); v1 serves text classification for the router's
// `classifier` condition type (issue #2592) on the CPU EP. The subprocess is a
// self-contained ort-server bundle, like moonshine-server.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "onnxruntime",
    /*display_name*/    "ONNX Runtime",
    /*binary*/          "ort-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"onnxruntime_args", "--onnxruntime-args", "", "ARGS",
         "Custom arguments to pass to ort-server", "ONNX Runtime Options"},
    },
    /*support*/ {
        {"cpu", {"windows"}, {{"cpu", {"x86_64"}}}, "x86_64 CPU"},
        {"cpu", {"linux"}, {{"cpu", {"x86_64", "arm64"}}}, "x86_64/arm64 CPU"},
        {"cpu", {"macos"}, {{"cpu", {"arm64"}}}, "arm64 CPU"},
    },
    /*default_labels*/  {"classification"},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text classification",
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {"cpu"},
    /*bin_variants*/    {"cpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace onnxruntime
}  // namespace backends
}  // namespace lemon
