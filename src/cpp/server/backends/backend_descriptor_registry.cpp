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

} // namespace backends
} // namespace lemon
