#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace kokoro {

// The kokoro backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "kokoro",
    /*display_name*/    "Kokoro",
#ifdef _WIN32
    /*binary*/          "koko.exe",
#else
    /*binary*/          "koko",
#endif
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_CPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {},
    /*support*/ {
        {"cpu", {"windows", "linux"}, {{"cpu", {"x86_64"}}}, "x86_64 CPU"},
        {"metal", {"macos"}, {{"metal", {}}}, "Apple Silicon GPU"},
    },
    /*default_labels*/  {"tts"},  // kokoro only does TTS; declare it so a label-less
                                   // user model is typed TTS, not LLM (catalog models
                                   // also carry "tts" explicitly in server_models.json)
    /*required_checkpoints*/ {"main"},
    /*modality*/        "Text-to-speech",
    /*experimental*/    false,
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"cpu"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace kokoro
}  // namespace backends
}  // namespace lemon
