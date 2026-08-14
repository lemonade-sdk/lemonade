#include "lemon/backends/backend_descriptor_registry.h"

#include <algorithm>
#include <utility>

// Generated from LEMON_BACKENDS at configure time. Defines
// lemon::backends::all_generated_descriptors() (descriptor data only).
#include "backend_descriptors_generated.h"

namespace lemon {
namespace backends {

const std::vector<const BackendDescriptor*>& all_descriptors() {
    static const std::vector<const BackendDescriptor*> kDescriptors = all_generated_descriptors();
    return kDescriptors;
}

const BackendDescriptor* descriptor_for(const std::string& recipe) {
    for (const BackendDescriptor* d : all_descriptors()) {
        if (d->recipe == recipe) {
            return d;
        }
    }
    return nullptr;
}

bool has_backend(const std::string& recipe) {
    return descriptor_for(recipe) != nullptr;
}

bool recipe_has_rocm_channels(const std::string& recipe) {
    const BackendDescriptor* d = descriptor_for(recipe);
    return d != nullptr && !d->rocm_channels.empty();
}

const std::vector<std::string>& supported_modes_for(const std::string& recipe) {
    static const std::vector<std::string> kNone;
    const BackendDescriptor* d = descriptor_for(recipe);
    return d != nullptr ? d->supported_modes : kNone;
}

const std::string& default_mode_for(const std::string& recipe) {
    static const std::string kChat = "chat";
    const std::vector<std::string>& modes = supported_modes_for(recipe);
    return modes.empty() ? kChat : modes.front();
}

namespace {

bool serves(const std::vector<std::string>& modes, ModelType wanted) {
    if (modes.empty()) return true;  // no descriptor: a collection routes itself
    for (const std::string& mode : modes) {
        ModelType served = ModelType::LLM;
        if (deployment_mode_of(mode, served) && served == wanted) return true;
    }
    return false;
}

bool unservable(const std::vector<std::string>& modes, const std::string& label) {
    ModelType claimed = ModelType::LLM;
    return deployment_mode_of(label, claimed) && !serves(modes, claimed);
}

} // namespace

bool backend_serves_mode(const std::string& recipe, ModelType mode) {
    return serves(supported_modes_for(recipe), mode);
}

std::string modality_display_for(const BackendDescriptor& descriptor) {
    static const std::pair<ModelType, std::string> kDisplay[] = {
        {ModelType::LLM, "Text generation"},
        {ModelType::EMBEDDING, "Embeddings"},
        {ModelType::RERANKING, "Reranking"},
        {ModelType::TRANSCRIPTION, "Speech-to-text"},
        {ModelType::TTS, "Text-to-speech"},
        {ModelType::IMAGE, "Image generation"},
        {ModelType::AUDIO_GENERATION, "Audio generation"},
        {ModelType::CLASSIFICATION, "Text classification"},
        {ModelType::MESH, "3D generation"},
    };
    if (descriptor.supported_modes.empty()) return "";
    ModelType mode = ModelType::LLM;
    if (!deployment_mode_of(descriptor.supported_modes.front(), mode)) return "";
    for (const auto& [key, display] : kDisplay) {
        if (key == mode) return display;
    }
    return "";
}

std::vector<std::string> ensure_deployment_label(std::vector<std::string>& labels,
                                                 const std::string& recipe) {
    const BackendDescriptor* d = descriptor_for(recipe);
    const std::vector<std::string> kNoModes;
    const std::vector<std::string>& modes = d != nullptr ? d->supported_modes : kNoModes;

    // A mode the backend cannot serve is not a mode. Dropping it here, rather
    // than correcting the resolved type later, is what keeps a model's labels
    // and its ModelType from disagreeing.
    std::vector<std::string> dropped;
    auto drop = std::remove_if(labels.begin(), labels.end(),
                               [&](const std::string& label) {
                                   return unservable(modes, label);
                               });
    dropped.assign(std::make_move_iterator(drop), std::make_move_iterator(labels.end()));
    labels.erase(drop, labels.end());

    ModelType mode = ModelType::LLM;
    if (!find_deployment_mode(labels, mode)) {
        labels.push_back(default_mode_for(recipe));
    }
    if (d != nullptr) {
        for (const std::string& capability : d->default_capabilities) {
            add_label_once(labels, capability);
        }
    }
    return dropped;
}

} // namespace backends
} // namespace lemon
