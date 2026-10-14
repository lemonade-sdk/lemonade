#include <lemon/models/registry_id.h>

#include <algorithm>

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
                                               // Deliberately empty: this host serves files through its
                                               // files API, and leaving it unset keeps today's behavior of
                                               // never fetching from it outside that API.
                                               ""};
    return descriptor;
}

// Static storage, so a descriptor pointer handed out by descriptor_for stays
// valid for the run.
const std::vector<RegistryDescriptor>& table() {
    static const std::vector<RegistryDescriptor> descriptors{huggingface(), modelscope()};
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

std::vector<RegistryDescriptor> all_registry_descriptors() { return table(); }

}  // namespace models
}  // namespace lemon
