#pragma once

#include "artifact_layout.h"
#include "fetchable_repository.h"
#include "artifact_source.h"
#include "registry_id.h"

#include <functional>
#include <memory>
#include <vector>
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
        std::function<std::unique_ptr<FetchableRepository>(
            const RegistryId& id, const ArtifactRef& ref, const SourcePlacement& placement)>
            repository_for;
        // default_model_source; empty is an error, not a default
        std::function<std::string()> default_registry_id;
    };

    explicit ArtifactSourceResolver(Hooks hooks = {});

    std::unique_ptr<ArtifactSource> source_for(const ModelInfo& info,
                                               const std::string& checkpoint_type) const;

    // Every checkpoint of a model that a repository holds, main first, with the repository
    // id and variant parsed out of the checkpoint string. A lazy checkpoint — one the backend
    // acquires for itself — is absent from the list rather than present and optional, which
    // is what makes a plan built from these refs the plan a pull would run.
    std::vector<ArtifactRef> refs_for(const ModelInfo& info) const;

    // The repository holding a ref, through the injected seam when the owner installed one.
    // Placement is a parameter because a probe may have no model row to read a recipe from.
    std::unique_ptr<FetchableRepository> repository_for(
        const ArtifactRef& ref, const SourcePlacement& placement) const;

// Which layout applies is the backend's fact, not the host's.
    const ArtifactLayout& layout_for(const ModelInfo& info,
                                     const std::string& checkpoint_type) const;

  private:
    FileValidator validator_for(const std::string& recipe) const;
    SourcePlacement placement_for(const ModelInfo& info,
                                  const std::string& checkpoint_type) const;
    std::unique_ptr<ArtifactSource> open_repository(const RegistryId& host,
                                                    const ArtifactRef& ref,
                                                    const SourcePlacement& placement) const;

    // The source a model belongs to when its checkpoints do not say.
    RegistryId default_host() const;

    Hooks hooks_;
};

}  // namespace models
}  // namespace lemon
