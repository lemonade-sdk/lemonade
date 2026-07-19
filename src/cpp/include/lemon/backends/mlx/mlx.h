#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace mlx {

// The lemon-mlx backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "lemon-mlx",
    /*display_name*/    "Lemon MLX (experimental)",
#ifdef _WIN32
    /*binary*/          "server.exe",
#else
    /*binary*/          "server",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"lemon-mlx_backend", "--lemon-mlx", "", "BACKEND",
         "lemon-mlx backend to use", "lemon-mlx Options"},
        {"lemon-mlx_args", "--lemon-mlx-args", "", "ARGS",
         "Custom arguments to pass to lemon-mlx server", "lemon-mlx Options"},
    },
    /*support*/ {
        {"metal", {"macos"}, { {"metal", {}} }, "Apple Silicon GPU"},
        {"rocm", {"linux"}, { {"amd_gpu", {"gfx1151"}} }, "AMD Strix Halo iGPU (gfx1151)"},
    },
    /*default_labels*/  {},
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text generation",
    /*experimental*/    true,
    /*web_display_name*/ "MLX",
    /*rocm_channels*/   {"stable"},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {},
    // Keep the CPU artifact and implementation available for development and
    // future optimization, but do not advertise it as a supported runtime path.
    /*bin_variants*/    {"metal", "rocm", "cpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace mlx
}  // namespace backends
}  // namespace lemon
