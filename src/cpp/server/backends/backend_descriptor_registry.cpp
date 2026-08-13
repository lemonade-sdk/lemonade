#include "lemon/backends/backend_descriptor_registry.h"

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

std::string default_classification_for(const std::string& recipe) {
    const BackendDescriptor* d = descriptor_for(recipe);
    if (d != nullptr && !d->default_classification.empty()) {
        return d->default_classification;
    }
    return "chat";
}

void ensure_deployment_label(std::vector<std::string>& labels, const std::string& recipe) {
    ModelType mode = ModelType::LLM;
    if (find_deployment_mode(labels, mode)) return;
    labels.push_back(default_classification_for(recipe));
}

} // namespace backends
} // namespace lemon
