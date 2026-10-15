#pragma once

#include "artifact_layout.h"
#include "fetchable_repository.h"
#include "artifact_source.h"
#include "registry_id.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace lemon {

struct ModelInfo;

namespace backends {
enum class DownloadPolicy : int;
}  // namespace backends

namespace models {

// A checkpoint binding to no source is broken, not merely absent.
class RegistrationError : public std::runtime_error {
  public:
    explicit RegistrationError(const std::string& message) : std::runtime_error(message) {}
};

// The only site that picks a source for a checkpoint.
class ArtifactSourceResolver {
  public:
    struct Hooks {
        std::function<backends::DownloadPolicy(const ModelInfo&)> download_policy;
        std::function<std::unique_ptr<FetchableRepository>(const RegistryId& id,
                                                           const ArtifactRef& ref)> repository_for;
        // default_model_source; empty is an error, not a default
        std::function<std::string()> default_registry_id;
        // fetched on first use, so absence does not make the model incomplete
        std::function<bool(const std::string& checkpoint_type)> acquired_lazily;
        std::function<std::string(const std::string& recipe, const std::string& path)>
            validate_file;
        // null falls back to the registered repositories
        std::function<const ArtifactLayout*(const std::string& recipe, const CheckpointRef&)>
            checkpoint_layout;
    };

    explicit ArtifactSourceResolver(Hooks hooks = {});

    std::unique_ptr<ArtifactSource> source_for(const ModelInfo& info,
                                               const std::string& checkpoint_type) const;

// Which layout applies is the backend's fact, not the host's.
    const ArtifactLayout& layout_for(const ModelInfo& info,
                                     const std::string& checkpoint_type) const;

  private:
    FileValidator validator_for(const std::string& recipe) const;
    void place(const ModelInfo& info, const std::string& checkpoint_type, ArtifactRef& ref) const;
    std::unique_ptr<ArtifactSource> open_repository(const RegistryId& host,
                                                    const ArtifactRef& ref) const;

    Hooks hooks_;
};

}  // namespace models
}  // namespace lemon
