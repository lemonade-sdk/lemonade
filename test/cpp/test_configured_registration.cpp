#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/backends/kokoro/kokoro_server.h"
#include "lemon/backends/moonshine/moonshine_server.h"
#include "lemon/backends/onnxruntime/onnxruntime_server.h"
#include "lemon/backends/trellis/trellis_server.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/backends/thinksound/thinksound_server.h"

#include <cstdlib>

using namespace thinksound_oracle;
using namespace lemon::backends;

namespace {
const char* const k_retired_optins[] = {"LEMONADE_THINKSOUND_CONFIGURED", "LEMONADE_DS4_CONFIGURED", "LEMONADE_RYZENAI_CONFIGURED"};
void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
uint32_t capability_bit(BackendOperation operation) {
    switch (operation) {
        case BackendOperation::Embeddings: return CAP_EMBEDDINGS;
        case BackendOperation::Reranking: return CAP_RERANKING;
        case BackendOperation::Classification: return CAP_CLASSIFICATION;
        case BackendOperation::Transcription: return CAP_TRANSCRIPTION;
        case BackendOperation::StreamingTranscription: return CAP_STREAMING_TRANSCRIPTION;
        case BackendOperation::TextToSpeech: return CAP_TTS;
        case BackendOperation::AudioGeneration: return CAP_AUDIO_GENERATION;
        case BackendOperation::ImageGeneration:
        case BackendOperation::ImageEdits:
        case BackendOperation::ImageVariations: return CAP_IMAGE;
        case BackendOperation::Upscale: return CAP_UPSCALE;
        case BackendOperation::Model3DGeneration: return CAP_MODEL_3D;
        default: return 0;
    }
}
uint32_t implemented_capabilities(const ConfiguredBackendEntry& entry) {
    uint32_t mask = 0;
    for (const auto& binding : entry.bindings) {
        if (binding.description.provenance == OperationProvenance::Implemented) mask |= capability_bit(binding.description.operation);
    }
    return mask;
}
uint32_t mode_capabilities(const ConfiguredBackendEntry& entry) {
    const std::map<std::string, uint32_t> modes = {{"embeddings", CAP_EMBEDDINGS}, {"reranking", CAP_RERANKING},
        {"transcription", CAP_TRANSCRIPTION | CAP_STREAMING_TRANSCRIPTION}, {"tts", CAP_TTS}, {"classification", CAP_CLASSIFICATION},
        {"image", CAP_IMAGE | CAP_UPSCALE}, {"audio-generation", CAP_AUDIO_GENERATION}, {"3d", CAP_MODEL_3D}};
    uint32_t mask = 0;
    for (const auto& mode : entry.descriptor.supported_modes) if (modes.count(mode)) mask |= modes.at(mode);
    return mask;
}
struct Registered {
    std::string recipe;
    std::shared_ptr<const ConfiguredBackendEntry> entry;
    const BackendSpec* spec;
    const BackendOps* ops;
    uint32_t capabilities;
};
}

int main() {
    for (int pass = 0; pass < 3; ++pass) {
        const std::vector<Registered> registered = {
            {"thinksound", thinksound::configured_entry(), thinksound::spec(), thinksound::ops(), thinksound::capabilities()},
            {"ds4", ds4::configured_entry(), ds4::spec(), ds4::ops(), ds4::capabilities()},
            {"ryzenai-llm", ryzenai::configured_entry(), ryzenai::spec(), ryzenai::ops(), ryzenai::capabilities()},
            {"llamacpp-hrx", hrx::configured_entry(), hrx::spec(), hrx::ops(), hrx::capabilities()},
            {"kokoro", kokoro::configured_entry(), kokoro::spec(), kokoro::ops(), kokoro::capabilities()},
            {"moonshine", moonshine::configured_entry(), moonshine::spec(), moonshine::ops(), moonshine::capabilities()},
            {"onnxruntime", onnxruntime::configured_entry(), onnxruntime::spec(), onnxruntime::ops(), onnxruntime::capabilities()},
            {"trellis", trellis::configured_entry(), trellis::spec(), trellis::ops(), trellis::capabilities()},
        };
        for (const auto& backend : registered) {
            auto server = create_server(backend.recipe, BackendContext{"error"});
            check(dynamic_cast<ConfiguredServer*>(server.get()) != nullptr, backend.recipe + " factory constructs the configured server");
            check(server && server->get_descriptor() == descriptor_for(backend.recipe), backend.recipe + " server carries the registry's descriptor object");
            check(backend.spec == &backend.entry->spec && backend.spec->definition == backend.entry->definition && backend.ops == backend.entry->backend_ops(), backend.recipe + " spec/ops are the configured entry's");
            check(spec_for(backend.recipe) == backend.spec && ops_for(backend.recipe) == backend.ops, backend.recipe + " registry lookups return the configured spec/ops");
            check(backend.capabilities == (implemented_capabilities(*backend.entry) & mode_capabilities(*backend.entry)), backend.recipe + " capability export matches the definition's implemented bindings within its declared modes");
        }
        bool shared_descriptors = builtin_registrations().size() == builtin_descriptors().size();
        for (size_t i = 0; shared_descriptors && i < builtin_descriptors().size(); ++i) {
            shared_descriptors = builtin_registrations()[i].descriptor == builtin_descriptors()[i] && descriptor_for(builtin_descriptors()[i]->recipe) == builtin_descriptors()[i];
        }
        check(shared_descriptors, "factory registrations and the descriptor registry share every descriptor object");
        check(thinksound::capabilities() == CAP_AUDIO_GENERATION && ds4::capabilities() == 0 && ryzenai::capabilities() == 0 && hrx::capabilities() == 0 && kokoro::capabilities() == CAP_TTS && moonshine::capabilities() == (CAP_TRANSCRIPTION | CAP_STREAMING_TRANSCRIPTION) && onnxruntime::capabilities() == CAP_CLASSIFICATION && trellis::capabilities() == CAP_MODEL_3D, "unchanged capability exports");
        check(thinksound::capabilities() == implemented_capabilities(*thinksound::configured_entry()) && ds4::capabilities() == implemented_capabilities(*ds4::configured_entry()) &&
              ryzenai::capabilities() == implemented_capabilities(*ryzenai::configured_entry()) && kokoro::capabilities() == implemented_capabilities(*kokoro::configured_entry()) &&
              moonshine::capabilities() == implemented_capabilities(*moonshine::configured_entry()) && onnxruntime::capabilities() == implemented_capabilities(*onnxruntime::configured_entry()) &&
              trellis::capabilities() == implemented_capabilities(*trellis::configured_entry()), "single-mode engines export exactly their implemented bindings");
        check(implemented_capabilities(*hrx::configured_entry()) == (CAP_EMBEDDINGS | CAP_RERANKING), "HRX implements embeddings and reranking that its chat-only mode does not advertise");
        check(thinksound::spec()->recipe == "thinksound" && thinksound::spec()->binary == "ts-server" && ds4::spec()->recipe == "ds4" && ds4::spec()->binary == "ds4-server", "alternating registration independent identities");
        check(ryzenai::spec()->recipe == "ryzenai-server" && ryzenai::spec()->binary == registered_descriptor("ryzenai-llm").binary, "RyzenAI split install recipe and platform binary");
        check(thinksound::configured_entry()->compatibility == TrustedCompatibility::LegacyNonChatSse && !thinksound::configured_entry()->launch_compatibility.device_on_backend_selection && !thinksound::configured_entry()->launch_compatibility.posix_executable_loader_path && !thinksound::configured_entry()->launch_compatibility.load_log, "ThinkSound carries only the legacy non-chat SSE attachment");
        check(ds4::configured_entry()->compatibility == TrustedCompatibility::None && ds4::configured_entry()->launch_compatibility.device_on_backend_selection == DEVICE_GPU && ds4::configured_entry()->launch_compatibility.posix_executable_loader_path && ds4::configured_entry()->launch_compatibility.load_log, "DS4 carries only the built-in launch attachment");
        check(ryzenai::configured_entry()->compatibility == TrustedCompatibility::None && !ryzenai::configured_entry()->launch_compatibility.device_on_backend_selection && !ryzenai::configured_entry()->launch_compatibility.posix_executable_loader_path && ryzenai::configured_entry()->launch_compatibility.load_log, "RyzenAI carries only the load-log attachment");
        for (const char* key : k_retired_optins) set_env(key, pass % 2 ? "0" : "1");
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks); return failures ? 1 : 0;
}
