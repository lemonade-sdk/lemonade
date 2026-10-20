#include "backend_definition_exports_internal.h"

#include <algorithm>

namespace lemon::backends {

const std::vector<HookAbiDescriptor>& installed_hook_abis() {
    static const std::vector<HookAbiDescriptor> descriptors = {
        {"kokoro.voice_index", "launch", 1, 1, {}},
        {"llamacpp.client_model", "response", 1, 1, {}},
        {"llamacpp.gguf", "artifacts", 1, 1, {}},
        {"llamacpp.max_tokens_alias", "request", 1, 1, {}},
        {"llamacpp.tool_schema_limits", "request", 1, 1, {}},
        {"moonshine.model_arch", "launch", 1, 1, {}},
        {"moonshine.transcription", "operation", 1, 1, {"transcription"}},
        {"onnxruntime.classify", "operation", 1, 1, {"classification"}},
        {"onnxruntime.model_directory", "artifacts", 1, 1, {}},
        {"onnxruntime.servable_directory", "launch", 1, 1, {}},
        {"onnxruntime.startup_error", "start_failure", 1, 1, {}},
        {"trellis.mesh", "sink_operation", 1, 1, {"model_3d_generation"}}
    };
    return descriptors;
}

const HookAbiDescriptor* find_hook_abi(std::string_view id) noexcept {
    const auto& descriptors = installed_hook_abis();
    const auto found = std::find_if(descriptors.begin(), descriptors.end(),
        [&](const auto& descriptor) { return descriptor.id == id; });
    return found == descriptors.end() ? nullptr : &*found;
}

const std::string& installed_hook_abi_digest() {
    static const std::string digest = v2::identity("lemonade.backend-hook-abi.v1\n", v2::hook_abi_json(installed_hook_abis()));
    return digest;
}

}
