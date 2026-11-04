#include <lemon/models/registry_id.h>

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace lemon {
namespace models {

namespace {

// A deque keeps a handed-out descriptor pointer valid across a later push_back,
// and writes happen only during static initialization, so readers need no lock.
std::deque<RegistryDescriptor>& table() {
    static std::deque<RegistryDescriptor> descriptors;
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

namespace {
// An absent row here means the repository translation unit was not linked into
// this binary, which no call site can recover from.
const RegistryDescriptor& require_builtin(const RegistryId& id) {
    const RegistryDescriptor* descriptor = descriptor_for(id);
    if (descriptor == nullptr) {
        throw std::runtime_error("No repository registered for '" + id.str() + "'");
    }
    return *descriptor;
}
}  // namespace

const RegistryDescriptor& huggingface_descriptor() {
    return require_builtin(RegistryId("huggingface"));
}

const RegistryDescriptor& modelscope_descriptor() {
    return require_builtin(RegistryId("modelscope"));
}

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
