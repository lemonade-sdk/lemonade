#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/ryzenai/ryzenai.h"
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
        };
        for (const auto& backend : registered) {
            auto server = create_server(backend.recipe, BackendContext{"error"});
            check(dynamic_cast<ConfiguredServer*>(server.get()) != nullptr, backend.recipe + " factory constructs the configured server");
            check(backend.spec == &backend.entry->spec && backend.spec->definition == backend.entry->definition && backend.ops == backend.entry->ops.get(), backend.recipe + " spec/ops are the configured entry's");
            check(spec_for(backend.recipe) == backend.spec && ops_for(backend.recipe) == backend.ops, backend.recipe + " registry lookups return the configured spec/ops");
            check(backend.capabilities == implemented_capabilities(*backend.entry), backend.recipe + " capability export matches the definition's implemented bindings");
        }
        check(thinksound::capabilities() == CAP_AUDIO_GENERATION && ds4::capabilities() == 0 && ryzenai::capabilities() == 0, "unchanged capability exports");
        check(thinksound::spec()->recipe == "thinksound" && thinksound::spec()->binary == "ts-server" && ds4::spec()->recipe == "ds4" && ds4::spec()->binary == "ds4-server", "alternating registration independent identities");
        check(ryzenai::spec()->recipe == "ryzenai-server" && ryzenai::spec()->binary == ryzenai::descriptor.binary, "RyzenAI split install recipe and platform binary");
        check(thinksound::configured_entry()->compatibility == TrustedCompatibility::LegacyNonChatSse && !thinksound::configured_entry()->launch_compatibility.device_on_backend_selection && !thinksound::configured_entry()->launch_compatibility.posix_executable_loader_path, "ThinkSound carries only the legacy non-chat SSE attachment");
        check(ds4::configured_entry()->compatibility == TrustedCompatibility::None && ds4::configured_entry()->launch_compatibility.device_on_backend_selection == DEVICE_GPU && ds4::configured_entry()->launch_compatibility.posix_executable_loader_path, "DS4 carries only the built-in launch attachment");
        check(ryzenai::configured_entry()->compatibility == TrustedCompatibility::None && !ryzenai::configured_entry()->launch_compatibility.device_on_backend_selection && !ryzenai::configured_entry()->launch_compatibility.posix_executable_loader_path, "RyzenAI carries no compiled attachment");
        for (const char* key : k_retired_optins) set_env(key, pass % 2 ? "0" : "1");
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks); return failures ? 1 : 0;
}
