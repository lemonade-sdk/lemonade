#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace ds4 {

// Arguments ds4_args must not carry. ds4-server parses left-to-right, so a
// later flag wins over the ones Lemonade sets: moving the port breaks readiness
// and proxying, rebinding the host exposes the backend, swapping the model
// desynchronises the router, and the backend-selection flags would run the
// child on a different device than the one Lemonade is tracking.
// True if ds4-rocm publishes a build for `os`. SystemInfo::backend_supports_arch
// validates the device constraints and arch gates but not the support row's
// supported_os, and the direct backend-install endpoint does not go through
// model filtering, so an unsupported OS would otherwise resolve an asset that
// does not exist.
inline bool publishes_for_os(const std::string& os);

inline const std::set<std::string>& reserved_custom_arg_flags() {
    static const std::set<std::string> flags = {
        "-m", "--model",
        "--host", "--port",
        "-c", "--ctx",
        "--backend", "--cpu", "--metal", "--rocm", "--cuda",
    };
    return flags;
}

// The ds4 backend descriptor (plain data). DS4 (DwarfStar) is antirez's
// self-contained DeepSeek V4 inference engine with an OpenAI-compatible HTTP
// server (ds4-server). Upstream publishes no binaries, releases or tags, so
// builds come from lemonade-sdk/ds4-rocm, which compiles a pinned upstream
// commit and bundles the ROCm runtime alongside it.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ds4",
    /*display_name*/    "DwarfStar4 (experimental)",
    /*binary*/          "ds4-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only variant; no option to declare
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"ds4_args", "--ds4-args", "", "ARGS",
         "Custom arguments to pass to ds4-server", "DS4 Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}}, "Prebuilt ds4 for AMD Strix Halo"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},  // single rocm artifact, no stable/nightly channels
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
    /*streams_model_from_storage*/ true,
};

inline bool publishes_for_os(const std::string& os) {
    for (const auto& row : descriptor.support) {
        if (row.backend == "rocm") {
            return row.supported_os.count(os) > 0;
        }
    }
    return false;
}

}  // namespace ds4
}  // namespace backends
}  // namespace lemon
