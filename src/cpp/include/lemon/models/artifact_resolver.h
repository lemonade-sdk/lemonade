#pragma once

#include "artifact_source.h"
#include "registry_id.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

// A checkpoint that binds to no source is broken, not merely absent.
class RegistrationError : public std::runtime_error {
  public:
    explicit RegistrationError(const std::string& message) : std::runtime_error(message) {}
};

// The only site that picks a source for a checkpoint.
class ArtifactSourceResolver {
  public:
    struct Hooks {
        std::function<bool(const std::string& recipe)> fetches_no_artifacts;
        std::function<bool(const std::string& recipe)> backend_self_manages_downloads;
        std::function<std::unique_ptr<ArtifactSource>(const RegistryId& id, const ArtifactRef& ref)>
            repository_for;
        // The default_model_source config key. Empty is an error, not a default.
        std::function<std::string()> default_registry_id;
        // Acquired on first use; absent bytes here leave the model downloaded.
        std::function<bool(const std::string& checkpoint_type)> acquired_lazily;
        std::function<std::string(const std::string& recipe, const std::string& path)>
            validate_file;
    };

    explicit ArtifactSourceResolver(Hooks hooks = {});

    std::unique_ptr<ArtifactSource> source_for(const ModelInfo& info,
                                               const std::string& checkpoint_type) const;

  private:
    FileValidator validator_for(const std::string& recipe) const;

    Hooks hooks_;
};

}  // namespace models
}  // namespace lemon
