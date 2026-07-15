#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

enum class RemoteRegistrySource {
    HuggingFace,
    ModelScope,
};

class RegistryNotFoundError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RegistryFile {
    std::string path;
    std::uint64_t size = 0;
    std::string hash_algorithm;
    std::string hash;
    bool directory = false;
};

struct RegistryRepository {
    std::string repo_id;
    std::string revision;      // revision used by the remote download endpoint
    std::string snapshot_id;   // immutable/local snapshot identifier stored in refs/main
    std::vector<RegistryFile> files;
    nlohmann::json raw_metadata;
};

// Normalized registry names persisted in user_models.json and exposed by APIs.
// Empty input intentionally maps to Hugging Face for backward compatibility.
RemoteRegistrySource parse_remote_registry_source(const std::string& source);
std::string remote_registry_source_name(RemoteRegistrySource source);
std::string remote_registry_display_name(RemoteRegistrySource source);
bool is_remote_registry_source(const std::string& source);

// Deterministic snapshot id for providers whose download revision may remain
// mutable. Ordering of files does not affect the result.
std::string registry_tree_snapshot_id(RemoteRegistrySource source,
                                      const std::string& revision,
                                      const std::vector<RegistryFile>& files);

// Provider-qualified cache directory. Hugging Face keeps its historical layout;
// ModelScope receives a distinct namespace so identical owner/repo ids never
// collide while the snapshot/refs layout below it stays runtime-compatible.
std::string registry_repo_cache_dir_name(const std::string& repo_id,
                                         RemoteRegistrySource source);

class ModelRegistry {
public:
    virtual ~ModelRegistry() = default;

    virtual RemoteRegistrySource source() const = 0;
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

const ModelRegistry& model_registry(RemoteRegistrySource source);
const ModelRegistry& model_registry(const std::string& source);

}  // namespace lemon
