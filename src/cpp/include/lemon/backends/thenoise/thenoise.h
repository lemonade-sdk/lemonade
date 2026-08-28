#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace thenoise {

// The thenoise backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "thenoise",
    /*display_name*/    "TheNoise ROCm",
    /*binary*/          "thenoise",
    /*config_section*/  "thenoise",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"thenoise_backend", "--thenoise", "", "BACKEND",
         "TheNoise backend to use", "TheNoise Options"},
        // Image generation defaults (recipe-level only, not CLI flags).
        {"steps", "", 20, "SIZE", "Number of denoising steps", "TheNoise Options"},
        {"cfg_scale", "", 7.0, "SIZE", "CFG scale (<= 1.0 disables CFG)", "TheNoise Options"},
        {"width", "", 512, "SIZE", "Output image width", "TheNoise Options"},
        {"height", "", 512, "SIZE", "Output image height", "TheNoise Options"},
        {"sampler", "", "", "ARGS", "Denoising solver (euler | er_sde)", "TheNoise Options"},
        {"negative_prompt", "", "", "ARGS", "Negative prompt", "TheNoise Options"},
        {"qwen_vae_enhance", "", false, "BOOL", "Nyquist notch post-filter (removes 2px grid artifacts)", "TheNoise Options"},
        {"film_grain", "", 0.0, "SIZE", "Film grain strength (0.0-10.0)", "TheNoise Options"},
        {"sharpening", "", 0.0, "SIZE", "RCAS sharpening strength (0.0-1.0)", "TheNoise Options"},
        {"lora_specs", "", "", "ARGS", "Comma-separated LoRA specs, e.g. \"style:0.8,sub/detail:0.5\"", "TheNoise Options"},
    },
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1150", "gfx1151", "gfx1152"}}}, "Supported AMD ROCm iGPU families"},
    },
    /*supported_modes*/ {"image"},
    /*required_checkpoints*/ {"main"},  // text_encoder+vae validated together in load()
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "thenoise",
    /*rocm_channels*/   {},  // single rocm artifact, no stable/nightly channels
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"rocm"},
    /*config_extra*/    {{"lora_dir", ""}, {"upscaler_dir", ""}},
};

}  // namespace thenoise
}  // namespace backends
}  // namespace lemon
