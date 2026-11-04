#pragma once

#include <lemon/models/registry_types.h>

#include <map>
#include <string>

#include <lemon/models/registry_id.h>

namespace lemon {

// A registry is named by string, not by enum: the string is what user_models.json,
// .lemonade_registry.json, POST /pull and the desktop app already carry.
using models::RegistryId;

// Search a remote model registry. Provider-side GGUF filters are candidate
// hints only; callers must use /pull/variants as the authoritative file-level
// compatibility check before presenting a downloadable marketplace result.
// Production callers reach this through FetchableRepository::search; the free
// shape stays for the adapter pair and its fixture tests.
RegistrySearchResponse search_registry_models(
    RegistryId source,
    const std::string& query,
    std::size_t limit = 12,
    bool gguf_only = false);

class ModelRegistry {
public:
    virtual ~ModelRegistry() = default;

    virtual RegistryId source() const = 0;
    virtual std::string default_revision() const = 0;
    virtual std::map<std::string, std::string> auth_headers() const = 0;

    // Fetch and normalize a repository file tree. Empty revision means the
    // provider default. snapshot_id is stable for unchanged content and changes
    // when the provider reports a new immutable revision/file tree.
    virtual RegistryRepository fetch_repository(
        const std::string& repo_id,
        const std::string& revision = "") const = 0;

    virtual std::string resolve_file_url(const std::string& repo_id,
                                         const std::string& revision,
                                         const std::string& file_path) const = 0;
};

const ModelRegistry& model_registry(RegistryId source);
const ModelRegistry& model_registry(const std::string& source);

}  // namespace lemon
