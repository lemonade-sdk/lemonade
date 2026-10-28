#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon::backends::strata {

inline const BackendDescriptor descriptor = {
    /*recipe*/ "strata",
    /*display_name*/ "Strata",
#ifdef _WIN32
    /*binary*/ "strata.exe",
#else
    /*binary*/ "strata",
#endif
    /*config_section*/ "",
    /*default_device*/ DEVICE_GPU,
    /*slot_policy*/ SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/ true,
    /*dynamic_models*/ false,
    /*options*/ {
        {"strata_backend", "--strata", "auto", "BACKEND",
         "Strata backend: auto, cuda, cuda13, rocm, metal", "Strata Options"},
        {"strata_args", "--strata-args", "", "ARGS",
         "Additional Strata engine arguments", "Strata Options"},
        {"strata_mtp", "--strata-mtp", true, "BOOL",
         "Enable model-compatible Strata MTP drafts (external pack or embedded GGUF block)", "Strata Options"},
        {"strata_vision", "--strata-vision", "cpu", "MODE",
         "Native picture reader: cpu, gpu (if packaged), off", "Strata Options"},
        {"strata_kv", "--strata-kv", "auto", "FORMAT",
         "Strata KV cache, where supported: auto, fp16, int8, q4_0, k8v4", "Strata Options"},
        {"strata_experts", "--strata-experts", "auto", "MODE",
         "Strata CPU experts: auto (model defaults), mmap or resident where supported", "Strata Options"},
        {"strata_kv_streaming", "--strata-kv-streaming", "auto", "MODE",
         "Strata RAM-backed KV streaming, where supported: auto, on, off", "Strata Options"},
    },
    /*support*/ {
        {"rocm", BackendTier::Experimental, BackendFormat::Native, {"windows", "linux"},
         {{"amd_gpu", {"gfx1030", "gfx1031", "gfx103X", "gfx1100", "gfx1101", "gfx1102",
                       "gfx110X", "gfx1151", "gfx1200", "gfx1201", "gfx120X"}}},
         "AMD RDNA2/3/4 and Strix Halo"},
        {"cuda", BackendTier::Experimental, BackendFormat::Native, {"windows", "linux"},
         {{"nvidia_gpu", {"sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120"}}},
         "NVIDIA GPUs (CUDA 12)"},
        {"cuda13", BackendTier::Experimental, BackendFormat::Native, {"windows", "linux"},
         {{"nvidia_gpu", {"sm_75", "sm_80", "sm_86", "sm_89", "sm_90", "sm_100", "sm_120"}}},
         "NVIDIA GPUs (CUDA 13, driver 580+)"},
        {"metal", BackendTier::Experimental, BackendFormat::Native, {"macos"},
         {{"metal", {}}}, "Apple Silicon (macOS 15+)"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "Strata",
    /*rocm_channels*/ {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/ VersionPolicy::Exact,
    /*self_manages_downloads*/ true,
    /*takes_args*/ true,
    /*arg_variants*/ {},
    /*bin_variants*/ {},
    /*config_extra*/ nlohmann::json::object(),
    /*streams_model_from_storage*/ true,
};

}
