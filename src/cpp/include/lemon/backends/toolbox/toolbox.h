#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <set>
#include <string>

namespace lemon {
namespace backends {
namespace toolbox {

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

// The llamacpp-toolbox descriptor. These are Donato Capitella's prebuilt
// llama.cpp container images for AMD Strix Halo (gfx1151) and Radeon AI PRO
// R9700 (gfx1201). Two variants track upstream llama.cpp on a stable channel;
// three are experimental forks that mainline llama.cpp cannot substitute for:
// ROCmFPX adds FP4/FP6/FP8 weight formats plus MTP, and vulkan-performance is
// Nathan W's Vulkan tuning fork.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "llamacpp-toolbox",
    /*display_name*/    "Llama.cpp Toolbox (experimental)",
    /*binary*/          "llama-server",
    /*config_section*/  "toolbox",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"llamacpp-toolbox_backend", "--llamacpp-toolbox", "", "BACKEND",
         "Toolbox image variant to run", "Llama.cpp Toolbox Options"},
        {"toolbox_args", "--toolbox-args", "", "ARGS",
         "Custom arguments to pass to the toolbox llama-server", "Llama.cpp Toolbox Options"},
    },
    // Only the forks. Stock llama.cpp on ROCm or Vulkan is what the `llamacpp`
    // recipe already does; these are the builds it cannot substitute for.
    /*support*/ {
        {"rocmfpx", {"linux"}, {{"amd_gpu", {"gfx1151"}}},
         "AMD Strix Halo (ROCmFP4/FP6/FP8 weights, MTP)"},
        {"nathanw", {"linux"}, {{"amd_gpu", {"gfx1151"}}},
         "AMD Strix Halo (Nathan W's Vulkan performance fork)"},
    },
    /*supported_modes*/ {"chat", "embeddings", "reranking"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "llama.cpp Toolbox",
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
    /*image_backed*/    true,
};

// Launch defaults carried over from the upstream catalog's calibrated serving
// configs. llama.cpp parses left to right, so a user's --toolbox-args copy of
// any of these still wins.
struct VariantDefaults {
    int batch_size;
    int ubatch_size;
    bool flash_attention;
    bool no_mmap;
};

inline VariantDefaults defaults_for(const std::string& variant) {
    // ROCmFPX loads FP4/FP6/FP8 weights that must stay resident to be decoded on
    // the fly, so mmap is disabled there; the calibrated batch/ubatch pair comes
    // from the catalog's Strix Halo serving configs.
    if (variant == "rocmfpx") {
        return {2048, 2048, true, true};
    }
    return {2048, 2048, true, false};
}

// Device passthrough profile per variant. Stated rather than derived from the
// name: "nathanw" is a Vulkan build whose name says nothing about that, and a
// name-prefix rule would silently hand it the ROCm profile.
inline const char* profile_for(const std::string& variant) {
    if (variant == "rocmfpx") return "amd-rocm";
    return "vulkan";
}

}  // namespace toolbox
}  // namespace backends
}  // namespace lemon
