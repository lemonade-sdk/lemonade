#include <lemon/models/registry_id.h>

#include <algorithm>
#include <deque>

namespace lemon {
namespace models {

namespace {

const RegistryDescriptor& huggingface() {
    static const RegistryDescriptor descriptor{RegistryId("huggingface"),
                                               {"huggingface", "hf", ""},
                                               "Hugging Face",
                                               "hf",
                                               "models--",
                                               "main",
                                               "https://huggingface.co"};
    return descriptor;
}

const RegistryDescriptor& modelscope() {
    static const RegistryDescriptor descriptor{RegistryId("modelscope"),
                                               {"modelscope", "ms"},
                                               "ModelScope",
                                               "ms",
                                               "modelscope--models--",
                                               "master",
                                               // Empty: no plain resolve URL for this host.
                                               ""};
    return descriptor;
}

// Written only during static initialization, so readers need no lock. A deque
// keeps a handed-out descriptor pointer valid across a later push_back.
std::deque<RegistryDescriptor>& table() {
    static std::deque<RegistryDescriptor> descriptors{huggingface(), modelscope()};
    return descriptors;
}

}  // namespace

std::optional<RegistryId> registry_id_from(const std::string& spelling) {
    std::string normalized = spelling;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& descriptor : table()) {
        if (std::find(descriptor.aliases.begin(), descriptor.aliases.end(), normalized) !=
            descriptor.aliases.end()) {
            return descriptor.id;
        }
    }
    return std::nullopt;
}

const RegistryDescriptor* descriptor_for(const RegistryId& id) {
    for (const auto& descriptor : table()) {
        if (descriptor.id == id) {
            return &descriptor;
        }
    }
    return nullptr;
}

const RegistryDescriptor& huggingface_descriptor() { return huggingface(); }

const RegistryDescriptor& modelscope_descriptor() { return modelscope(); }

std::vector<RegistryDescriptor> all_registry_descriptors() {
    return {table().begin(), table().end()};
}

namespace detail {

void add_descriptor(const RegistryDescriptor& descriptor) {
    for (auto& existing : table()) {
        if (existing.id == descriptor.id) {
            existing = descriptor;
            return;
        }
    }
    table().push_back(descriptor);
}

}  // namespace detail

}  // namespace models
}  // namespace lemon
