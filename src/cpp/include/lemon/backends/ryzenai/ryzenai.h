#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace ryzenai {

// The ryzenai backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "ryzenai-llm",
    /*display_name*/    "Ryzen AI LLM",
#ifdef _WIN32
    /*binary*/          "ryzenai-server.exe",
#else
    /*binary*/          "ryzenai-server",
#endif
    /*config_section*/  "ryzenai",  // differs from recipe "ryzenai-llm"
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"npu", BackendTier::Core, BackendFormat::Native, {"windows"}, {{"amd_npu", {"XDNA2"}}}, "XDNA2 NPU"},
    },
    /*about*/ {
        {"npu", {"AMD's Ryzen AI software stack running ONNX Runtime GenAI LLMs on the NPU.",
                 "AMD", "lemonade-sdk", "MIT (server) and AMD EULA (Ryzen AI libraries)",
                 "https://github.com/lemonade-sdk/ryzenai-server"}},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "Ryzen AI SW NPU",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"server"},
    /*config_extra*/    nlohmann::json::object(),
};

// The recipe ships -CPU, -NPU and -Hybrid variants of the same model family, but
// the descriptor declares a single default device (NPU) for all of them. This
// maps a model name to the device that variant actually uses, so /api/v1/health
// does not report every ryzenai-llm model as NPU-only (#4170).
inline DeviceType device_for_variant(const std::string& model_name) {
    const auto ends_with = [&model_name](const char* suffix) {
        const std::string tail(suffix);
        return model_name.size() >= tail.size() &&
               model_name.compare(model_name.size() - tail.size(), tail.size(), tail) == 0;
    };

    if (ends_with("-Hybrid")) {
        // Hybrid runs the model on the NPU with the iGPU alongside it.
        return DEVICE_NPU | DEVICE_GPU;
    }

    if (ends_with("-CPU")) {
        return DEVICE_CPU;
    }

    // -NPU, and any name without a variant suffix: this recipe only runs on the NPU.
    return DEVICE_NPU;
}

}  // namespace ryzenai
}  // namespace backends
}  // namespace lemon
