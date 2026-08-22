#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace openmoss {

inline const BackendDescriptor descriptor = {
    /*recipe*/          "openmoss",
    /*display_name*/    "OpenMOSS TTS",
    /*binary*/          "moss-tts-server",
    /*config_section*/  "",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"openmoss_backend", "--openmoss", "", "BACKEND",
         "OpenMOSS TTS backend to use", "Text-to-Speech Options"},
    },
    /*support*/ {
        {"cuda", {"linux", "windows"}, {{"nvidia_gpu", {}}}, "NVIDIA GPUs"},
        {"vulkan", {"linux", "windows"}, {{"cpu", {"x86_64"}}, {"amd_gpu", {}}, {"nvidia_gpu", {}}}, "Vulkan-capable GPUs"},
        {"rocm", {"linux", "windows"}, {{"amd_gpu", {}}}, "AMD GPUs (ROCm via TheRock)"},
    },
    /*supported_modes*/ {"tts", "audio-generation"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "",
    /*rocm_channels*/   {"stable"},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {"vulkan", "rocm", "cuda"},
    /*config_extra*/    nlohmann::json::object(),
    /*generation_params*/ {
        {"voice_design_description", "tts", "Describe voice", "TEXT", nullptr, nullptr, nullptr, nullptr, {},
         "Lemonade renders a short sample in the described voice and speaks with it.", "", "voice_mode", "", nullptr},
        {"reference_wav_b64", "tts", "Clone WAV sample", "AUDIO_B64", nullptr, nullptr, nullptr, nullptr, {},
         "One WAV sample whose voice is cloned.", "", "voice_mode", ".wav", nullptr},
        {"voice", "tts", "Style note", "TEXT", nullptr, nullptr, nullptr, nullptr, {},
         "Optional delivery instruction; does not change the timbre.", "", "", "", nullptr},
        {"audio_temperature", "tts", "Audio temp", "NUMBER", 1.7, 0.0, 3.0, 0.05, {}, "", "advanced", "", "", nullptr},
        {"audio_top_p", "tts", "Audio top-p", "NUMBER", 0.8, 0.0, 1.0, 0.05, {}, "", "advanced", "", "", nullptr},
        {"audio_top_k", "tts", "Audio top-k", "INT", 25, 0, 200, 1, {}, "", "advanced", "", "", nullptr},
        {"audio_repetition_penalty", "tts", "Repetition", "NUMBER", 1.0, 1.0, 2.0, 0.05, {}, "", "advanced", "", "", nullptr},
        {"text_temperature", "tts", "Text temp", "NUMBER", 1.5, 0.0, 3.0, 0.05, {}, "", "advanced", "", "", nullptr},
        {"text_top_p", "tts", "Text top-p", "NUMBER", 1.0, 0.0, 1.0, 0.05, {}, "", "advanced", "", "", nullptr},
        {"text_top_k", "tts", "Text top-k", "INT", 50, 0, 200, 1, {}, "", "advanced", "", "", nullptr},
        {"speed", "tts", "Speed", "NUMBER", 1.0, 0.25, 4.0, 0.05, {}, "", "advanced", "", "", nullptr},

        {"seconds", "audio-generation", "Duration", "NUMBER", 10, 1, 300, 1, {}, "", "", "", "", nullptr},
        {"steps", "audio-generation", "Steps", "INT", 100, 1, 200, 1, {}, "", "", "", "", nullptr},
        {"cfg_scale", "audio-generation", "CFG", "NUMBER", 4.0, 0.0, 30.0, 0.5, {}, "", "", "", "", nullptr},
        {"sigma_shift", "audio-generation", "Sigma shift", "NUMBER", 5.0, 0.0, 20.0, 0.5, {}, "", "", "", "", nullptr},
        {"negative_prompt", "audio-generation", "Negative prompt", "TEXT", nullptr, nullptr, nullptr, nullptr, {},
         "", "", "", "", nullptr},
        {"seed", "audio-generation", "Seed", "SEED", nullptr, 0, 4294967295, 1, {},
         "Unsigned; 0 is a real seed, so a blank box draws a random one.", "", "", "", nullptr},
    },
};

}  // namespace openmoss
}  // namespace backends
}  // namespace lemon
