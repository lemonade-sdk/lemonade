#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// A pullable model host, named by string rather than by enum. The string is what
// user_models.json `registry_source`, .lemonade_registry.json `source`,
// POST /pull `source` and --source already carry, so a new host is one more
// accepted value in those places rather than a new enum member.
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

// Everything about a host that is not protocol: the spellings it answers to and
// the URL and on-disk shapes it dictates.
struct RegistryDescriptor {
    RegistryId id;
    std::vector<std::string> aliases;  // accepted spellings, lower case
    std::string display_name;
    std::string scheme_prefix;      // short form in a machine-minted id
    std::string cache_dir_prefix;   // provider-qualified namespace in the models dir
    std::string default_revision;
};

// Resolves any accepted spelling. Empty means the default host, which is what
// entries written before a source was recorded look like.
std::optional<RegistryId> registry_id_from(const std::string& spelling);

// Null for an id no registered host claims. A persisted entry naming one warns
// and contributes nothing; startup does not abort.
const RegistryDescriptor* descriptor_for(const RegistryId& id);

const RegistryDescriptor& huggingface_descriptor();
const RegistryDescriptor& modelscope_descriptor();

std::vector<RegistryDescriptor> all_registry_descriptors();

}  // namespace models
}  // namespace lemon
