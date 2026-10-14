#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// A pullable model host, named by the string every persisted form already carries.
class RegistryId {
  public:
    RegistryId() = default;

    explicit RegistryId(std::string canonical) : canonical_(std::move(canonical)) {}

    const std::string& str() const { return canonical_; }
    bool empty() const { return canonical_.empty(); }

    bool operator==(const RegistryId& other) const { return canonical_ == other.canonical_; }
    bool operator!=(const RegistryId& other) const { return !(*this == other); }
    bool operator<(const RegistryId& other) const { return canonical_ < other.canonical_; }

  private:
    std::string canonical_;
};

// How a host serves files; a backend refuses a protocol it cannot express.
namespace download_protocols {
inline constexpr const char hf_hub[] = "hf-hub";
inline constexpr const char modelscope_api[] = "modelscope-api";
}  // namespace download_protocols

// Per-host facts a caller may ask without naming the host.
struct RegistryDescriptor {
    RegistryId id;
    std::vector<std::string> aliases;  // accepted spellings, lower case
    std::string display_name;
    std::string scheme_prefix;      // short form in a machine-minted id
    std::string cache_dir_prefix;   // provider-qualified namespace in the models dir
    std::string default_revision;
    // Empty when the host serves files only through its own API.
    std::string endpoint_default;
    std::string download_protocol;
};

// Resolves any accepted spelling. Empty means the default host.
std::optional<RegistryId> registry_id_from(const std::string& spelling);

// Null when no registered host claims the id.
const RegistryDescriptor* descriptor_for(const RegistryId& id);

namespace detail {
// Registration is the only writer. A row with the same id is replaced.
void add_descriptor(const RegistryDescriptor& descriptor);
}  // namespace detail

const RegistryDescriptor& huggingface_descriptor();
const RegistryDescriptor& modelscope_descriptor();

std::vector<RegistryDescriptor> all_registry_descriptors();

}  // namespace models
}  // namespace lemon

// Static initializer. Nothing here may throw: an unrecognized host must not
// abort startup.
#define LEMONADE_REGISTER_REGISTRY_DESCRIPTOR(name, descriptor_expr)                 \
    static const bool lemonade_descriptor_registered_##name = [] {                   \
        ::lemon::models::detail::add_descriptor(descriptor_expr);                    \
        return true;                                                                 \
    }()
