#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace halogen {

// Halogen Flash is Peonist's closed-source inference engine with hand-written
// gfx1151 kernels. It is not one of Donato Capitella's toolboxes: it ships as
// its own container from ghcr.io, runs a single model family (Qwen3.8-Flash-Next
// as an HGN checkpoint plus an overlay), and is configured entirely through
// HALOGEN_* environment variables rather than a command line.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "halogen",
    /*display_name*/    "Halogen Flash (experimental)",
    /*binary*/          "",  // the image ships its own entrypoint
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,  // rocm is the only variant
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"rocm", {"linux"}, {{"amd_gpu", {"gfx1151"}}},
         "AMD Strix Halo only (hand-written gfx1151 kernels)"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "Halogen Flash",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ true,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
    // The HGN checkpoint is memory-mapped read-only and registered with the GPU
    // in place, never copied, so the 115 GiB file is a disk requirement rather
    // than a resident one. What must fit in the GPU's pool is the KV pool and
    // the overlay, which is what each model entry's min_resident_gb records.
    /*streams_model_from_storage*/ true,
    /*disable_thinking*/
    nlohmann::json{{"chat_template_kwargs", {{"enable_thinking", false}}}},
};

// The checkpoint is registered with the GPU as a read-only file mapping, and
// that registration needs kernel support that is not backported. Upstream
// reports every working install on 7.0.0 or later.
constexpr int kMinKernelMajor = 7;

}  // namespace halogen
}  // namespace backends
}  // namespace lemon
