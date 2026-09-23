#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace rocmfpx {

// Flags Lemonade owns on the container's llama-server. A later flag wins in
// llama.cpp's left-to-right parse, so a user-supplied copy of any of these would
// move the port out from under the proxy, rebind the host, or swap the model.
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

// ROCm FPX is a llama.cpp fork that adds FP4/FP6/FP8 weight formats plus MTP.
// Mainline llama.cpp cannot read those weights, which is what makes it its own
// engine rather than a build of the llamacpp recipe: its models run here and
// nowhere else. It ships as a prebuilt gfx1151 image from Donato Capitella's
// toolbox collection, which is packaging rather than anything a user chooses.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "rocmfpx",
    /*display_name*/    "ROCm FPX (experimental)",
    /*binary*/          "llama-server",
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // one build; the weight format is the model's, not the user's
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"rocmfpx_args", "--rocmfpx-args", "", "ARGS",
         "Custom arguments to pass to the ROCm FPX llama-server", "ROCm FPX Options"},
    },
    /*support*/ {
        {"rocmfpx", {"linux"}, {{"amd_gpu", {"gfx1151"}}},
         "AMD Strix Halo (ROCmFP4/FP6/FP8 weights, MTP)"},
    },
    /*supported_modes*/ {"chat", "embeddings", "reranking"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "ROCm FPX",
    /*rocm_channels*/   {},  // image variants are named directly, not by channel
    /*exposes_prometheus_metrics*/ true,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
    /*streams_model_from_storage*/ false,
};

// Launch defaults carried over from the upstream catalog's calibrated serving
// configs. llama.cpp parses left to right, so a user's --rocmfpx-args copy of
// any of these still wins. FP4/FP6/FP8 weights are decoded on the fly and must
// stay resident, so mmap is disabled.
struct LaunchDefaults {
    int batch_size;
    int ubatch_size;
    bool flash_attention;
    bool no_mmap;
};

inline LaunchDefaults launch_defaults() {
    return {2048, 2048, true, true};
}

}  // namespace rocmfpx
}  // namespace backends
}  // namespace lemon
