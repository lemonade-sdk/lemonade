// Proves each backend descriptor's declared `supported_modes` matches the
// capability interfaces its server class actually implements. A descriptor that
// advertises a mode with no code behind it would register models that fail at
// inference time with an unsupported-capability error.

#include "backend_capabilities_generated.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/model_types.h"
#include "lemon/server_capabilities.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using lemon::ModelType;
using lemon::deployment_mode_of;

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Each capability interface that names a deployment mode, and the label it maps
// to. ICompletionServer is deliberately absent: WrappedServer inherits it and
// gives every backend an "unsupported capability" default, so `chat` cannot be
// derived from the type system. It is checked separately below.
const std::vector<std::pair<uint32_t, std::string>> kModeInterfaces = {
    {lemon::CAP_EMBEDDINGS, "embeddings"},
    {lemon::CAP_RERANKING, "reranking"},
    {lemon::CAP_TRANSCRIPTION, "transcription"},
    {lemon::CAP_TTS, "tts"},
    {lemon::CAP_CLASSIFICATION, "classification"},
    {lemon::CAP_IMAGE, "image"},
    {lemon::CAP_AUDIO_GENERATION, "audio-generation"},
    {lemon::CAP_MODEL_3D, "3d"},
};

// Capabilities that name no deployment mode, so kModeInterfaces omits them by
// design rather than by oversight. Streaming transcription is a transport
// detail of the transcription mode, not a mode of its own.
constexpr uint32_t kNonModeCapabilities = lemon::CAP_STREAMING_TRANSCRIPTION;

// Serving one of these means a single fixed modality, which rules out chat.
// Transcription is absent because a backend can serve it alongside chat. Defined
// by subtraction so a newly added media capability is exclusive until someone
// deliberately lists it as chat-compatible.
constexpr uint32_t kChatCompatible = lemon::CAP_EMBEDDINGS | lemon::CAP_RERANKING |
                                     lemon::CAP_TRANSCRIPTION |
                                     lemon::CAP_STREAMING_TRANSCRIPTION;
constexpr uint32_t kExclusiveModalities = lemon::CAP_ALL & ~kChatCompatible;

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out.empty() ? "(none)" : out;
}

} // namespace

int main() {
    const auto entries = lemon::backends::all_backend_capabilities();
    check("registry exposes at least one backend", !entries.empty());

    // A capability added to the enum but classified nowhere would otherwise make
    // the mode contract quietly stop covering it.
    uint32_t classified = kNonModeCapabilities;
    for (const auto& [flag, mode] : kModeInterfaces) classified |= flag;
    check("every declared capability is classified as a mode or a non-mode",
          classified == lemon::CAP_ALL);

    for (const auto& entry : entries) {
        const lemon::BackendDescriptor& desc = *entry.descriptor;
        const std::string& recipe = desc.recipe;
        const std::set<std::string> declared(desc.supported_modes.begin(),
                                             desc.supported_modes.end());

        check(recipe + ": declares at least one mode", !desc.supported_modes.empty());
        check(recipe + ": declares no duplicate modes",
              declared.size() == desc.supported_modes.size());

        // Every declared mode must be a label find_deployment_mode() recognizes,
        // since these strings are compared against model labels verbatim.
        for (const auto& mode : desc.supported_modes) {
            ModelType type = ModelType::LLM;
            check(recipe + ": '" + mode + "' is a known deployment-mode label",
                  deployment_mode_of(mode, type));
        }

        // The contract: declared modes and implemented interfaces agree, in both
        // directions, for every mode the type system can see.
        for (const auto& [flag, mode] : kModeInterfaces) {
            const bool implemented = (entry.capabilities & flag) != 0;
            const bool advertised = declared.count(mode) != 0;
            check(recipe + ": '" + mode + "' " +
                      (implemented ? "implemented" : "not implemented") + " and " +
                      (advertised ? "declared" : "not declared") + " agree",
                  implemented == advertised);
        }

        // `chat` is the one mode with no interface of its own. What is still
        // checkable: a backend locked to a fixed non-chat modality must not
        // claim it.
        if (declared.count("chat") != 0) {
            check(recipe + ": declares 'chat' and implements no exclusive modality",
                  (entry.capabilities & kExclusiveModalities) == 0);
        }

        // Every mode has a human-readable name, so a new one cannot reach the
        // generated docs as a blank cell.
        check(recipe + ": default mode has a display name",
              !lemon::backends::modality_display_for(desc).empty());

        // Capability labels are not modes; stamping one must never change the
        // mode a bare model of this recipe resolves to.
        for (const auto& capability : desc.default_capabilities) {
            ModelType type = ModelType::LLM;
            check(recipe + ": capability '" + capability + "' is not a deployment mode",
                  !deployment_mode_of(capability, type));
        }

        std::printf("     %s: modes [%s] capabilities [%s]\n", recipe.c_str(),
                    join(desc.supported_modes).c_str(),
                    join(desc.default_capabilities).c_str());
    }

    // Ingest normalization: a model's labels and its resolved ModelType agree by
    // construction, because a mode claim the backend cannot serve is dropped
    // before the mode is read rather than corrected afterwards.
    struct IngestCase {
        const char* name;
        std::string recipe;
        std::vector<std::string> labels;
        std::vector<std::string> expect_present;
        std::vector<std::string> expect_absent;
        ModelType expect_type;
        bool expect_unservable;
    };

    const std::vector<IngestCase> ingest = {
        // An LLM-as-classifier for a router collection. llamacpp cannot answer
        // /v1/classify, so the claim is refused; the model is a chat model and
        // must carry `chat` or it drops out of every chat-model picker.
        {"llamacpp + classification", "llamacpp", {"classification"},
         {"chat"}, {"classification"}, ModelType::LLM, true},
        // The mirror image: a mode label the backend cannot serve must not
        // promote an image model into the chat path.
        {"sd-cpp + chat", "sd-cpp", {"chat"},
         {"image"}, {"chat"}, ModelType::IMAGE, true},
        // A mode the backend does serve is left alone — llamacpp embedding
        // models must keep deploying as embeddings.
        {"llamacpp + embeddings", "llamacpp", {"embeddings"},
         {"embeddings"}, {"chat"}, ModelType::EMBEDDING, false},
        {"llamacpp bare", "llamacpp", {}, {"chat"}, {}, ModelType::LLM, false},
        {"sd-cpp bare", "sd-cpp", {}, {"image"}, {"chat"}, ModelType::IMAGE, false},
        // onnxruntime is the backend that does serve classification.
        {"onnxruntime + classification", "onnxruntime", {"classification"},
         {"classification"}, {"chat"}, ModelType::CLASSIFICATION, false},
        // Capability labels ride along with the default mode.
        {"whispercpp bare", "whispercpp", {},
         {"transcription", "realtime-transcription"}, {"chat"},
         ModelType::TRANSCRIPTION, false},
        // Collections have no backend to reject anything: routing is the
        // collection's business, so any declared mode stands.
        {"collection + transcription", "collection.omni", {"transcription"},
         {"transcription"}, {"chat"}, ModelType::TRANSCRIPTION, false},
    };

    for (const auto& c : ingest) {
        std::vector<std::string> labels = c.labels;
        std::vector<std::string> dropped =
            lemon::backends::ensure_deployment_label(labels, c.recipe);
        check(std::string(c.name) + ": reports " +
                  (c.expect_unservable ? "a dropped mode" : "nothing dropped"),
              !dropped.empty() == c.expect_unservable);
        for (const auto& expected : c.expect_present) {
            check(std::string(c.name) + ": keeps '" + expected + "'",
                  std::find(labels.begin(), labels.end(), expected) != labels.end());
        }
        for (const auto& forbidden : c.expect_absent) {
            check(std::string(c.name) + ": drops '" + forbidden + "'",
                  std::find(labels.begin(), labels.end(), forbidden) == labels.end());
        }
        const ModelType type = lemon::get_model_type_from_labels(labels);
        check(std::string(c.name) + ": deploys as " +
                  lemon::model_type_to_string(c.expect_type),
              type == c.expect_type);

        // The guarantee clients depend on (#3070): anything deploying as an LLM
        // says so with `chat`, so "is this a chat model" is one label test and
        // never an inference from which labels happen to be absent.
        check(std::string(c.name) + ": LLM carries 'chat'",
              (type == ModelType::LLM) ==
                  (std::find(labels.begin(), labels.end(), "chat") != labels.end()));
    }

    // Every backend that serves the Realtime API declares it, so the desktop and
    // web apps can offer the microphone without probing the backend.
    for (const auto& entry : entries) {
        if ((entry.capabilities & lemon::CAP_STREAMING_TRANSCRIPTION) == 0) continue;
        const auto& caps = entry.descriptor->default_capabilities;
        check(entry.descriptor->recipe +
                  ": implements streaming transcription and declares "
                  "'realtime-transcription'",
              std::find(caps.begin(), caps.end(), "realtime-transcription") != caps.end());
    }

    if (failures == 0) {
        std::printf("\nAll backend mode contract checks passed.\n");
        return 0;
    }
    std::printf("\n%d backend mode contract check(s) failed.\n", failures);
    return 1;
}
