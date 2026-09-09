#include "lemon/backends/backend_descriptor_registry.h"

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

std::string quote_join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ", ";
        out += "'" + item + "'";
    }
    return out;
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

std::string illegal_deployment_labels(const std::vector<std::string>& labels,
                                      const std::string& recipe) {
    const std::vector<std::string>& modes = supported_modes_for(recipe);

    std::string deploys_as;
    ModelType deployed = ModelType::LLM;
    for (const std::string& label : labels) {
        ModelType claimed = ModelType::LLM;
        if (!deployment_mode_of(label, claimed)) continue;

        if (!serves(modes, claimed)) {
            return "recipe '" + recipe + "' cannot serve '" + label + "'. It serves " +
                   quote_join(modes) + ". Omit the label to deploy as '" +
                   default_mode_for(recipe) + "'.";
        }
        if (deploys_as.empty()) {
            deploys_as = label;
            deployed = claimed;
        } else if (claimed != deployed) {
            // The subprocess is spawned for one mode and configured for it at
            // load time (llama-server's --embeddings, --reranking), so the
            // second mode would name an endpoint it was never set up to answer.
            return "a model deploys in exactly one mode, but these labels name "
                   "two: '" + deploys_as + "' and '" + label +
                   "'. Register one model per mode.";
        }
    }
    return "";
}

void ensure_deployment_label(std::vector<std::string>& labels,
                             const std::string& recipe) {
    ModelType mode = ModelType::LLM;
    if (!find_deployment_mode(labels, mode)) {
        labels.push_back(default_mode_for(recipe));
    }
    const BackendDescriptor* d = descriptor_for(recipe);
    if (d != nullptr) {
        for (const std::string& capability : d->default_capabilities) {
            add_label_once(labels, capability);
        }
    }
}

} // namespace backends
} // namespace lemon
