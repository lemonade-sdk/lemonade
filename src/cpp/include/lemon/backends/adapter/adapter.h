#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace adapter {

// The adapter backend descriptor (plain data). One in-tree backend for engines that live out of tree:
// each engine is a JSON spec in <config dir>/adapters/ (or LEMONADE_ADAPTERS_DIR), read at load time,
// so adding or fixing an engine never changes Lemonade. A model names its spec in its checkpoint,
// "<spec>::<model path>", for example "polystrata::/models/LFM2.5-24B-A2B-QAD-Q4_0.gguf".
// Unlike a cloud provider, lemond starts and stops the engine, counts it in the slot pool (so it is
// evicted like any local model) and probes its health.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "adapter",
    /*display_name*/    "Engine adapter (out of tree)",
    /*binary*/          "",  // per engine, from its spec
    /*config_section*/  "",  // defaults to recipe
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,  // local memory: counts toward slots, evicted by LRU
    /*selectable_backend*/ false,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"adapter_args", "--adapter-args", "", "ARGS",
         "Replaces the spec's argument template ({model} {port} {device} {spec_dir})", "Adapter Options"},
        {"adapter_device", "--adapter-device", "", "DEVICE",
         "Device index or GPU UUID substituted for {device} (default: the spec's, else 0)", "Adapter Options"},
        {"adapter_telemetry", "--adapter-telemetry", "", "on|off",
         "Turn on the telemetry the spec declares (off unless the spec enables it)", "Adapter Options"},
    },
    /*support*/ {},  // no local gating: each spec says what it needs; availability is decided at load
    /*about*/ {},    // one entry per backend named in `support`, and there are none
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::AtLeast,
    /*self_manages_downloads*/ true,  // the model is a local path the engine reads itself
};

}  // namespace adapter
}  // namespace backends
}  // namespace lemon
