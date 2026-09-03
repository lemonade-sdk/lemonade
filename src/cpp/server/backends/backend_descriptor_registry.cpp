#include "lemon/backends/backend_descriptor_registry.h"

#include <cctype>
#include <string>
#include <utility>
#include "lemon/utils/origin_utils.h"

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

ModelCompatibility model_compatibility(const std::vector<ModelConstraint>& constraints,
                                       const std::string& architecture,
                                       const std::string& quant) {
    if (constraints.empty()) {
        return ModelCompatibility::Supported;
    }
    if (architecture.empty()) {
        return ModelCompatibility::Unknown;
    }

    const auto fold = [](const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
            if (ch == '_' || ch == '-' || ch == ' ') continue;
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    };

    const std::string arch = fold(architecture);
    const std::string token = utils::to_lower(quant);
    for (const ModelConstraint& c : constraints) {
        if (fold(c.architecture) != arch) continue;
        if (c.quants.empty()) return ModelCompatibility::Supported;
        if (token.empty()) return ModelCompatibility::Unknown;
        for (const std::string& allowed : c.quants) {
            if (utils::to_lower(allowed) == token) return ModelCompatibility::Supported;
        }
    }
    return ModelCompatibility::Unsupported;
}

bool model_constraints_allow(const std::vector<ModelConstraint>& constraints,
                             const std::string& architecture,
                             const std::string& quant) {
    return model_compatibility(constraints, architecture, quant) != ModelCompatibility::Unsupported;
}

std::string model_load_refusal(const std::string& recipe,
                               const std::string& architecture,
                               const std::string& quant) {
    const BackendDescriptor* d = descriptor_for(recipe);
    if (d == nullptr || d->supported_models.empty()) return "";

    const ModelCompatibility verdict =
        model_compatibility(d->supported_models, architecture, quant);
    if (verdict == ModelCompatibility::Supported) return "";

    const std::string serves = model_constraint_summary(recipe);
    if (verdict == ModelCompatibility::Unknown) {
        return "cannot confirm this model is compatible with the '" + recipe +
               "' backend, which serves: " + serves +
               " (architecture or quantization could not be determined)";
    }
    return "architecture " + (architecture.empty() ? std::string("unknown") : architecture) +
           (quant.empty() ? std::string() : " / " + quant) +
           " is not supported by the '" + recipe + "' backend, which serves: " + serves;
}

bool backend_supports_model(const std::string& recipe,
                            const std::string& architecture,
                            const std::string& quant) {
    const BackendDescriptor* d = descriptor_for(recipe);
    if (d == nullptr) {
        return true;
    }
    return model_constraints_allow(d->supported_models, architecture, quant);
}

std::string model_constraint_summary(const std::string& recipe) {
    const BackendDescriptor* d = descriptor_for(recipe);
    if (d == nullptr || d->supported_models.empty()) {
        return "";
    }
    std::string out;
    for (const ModelConstraint& c : d->supported_models) {
        if (!out.empty()) out += ", ";
        out += c.architecture;
        if (!c.quants.empty()) {
            out += " (";
            bool first = true;
            for (const std::string& q : c.quants) {
                if (!first) out += "/";
                out += q;
                first = false;
            }
            out += ")";
        }
    }
    return out;
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
