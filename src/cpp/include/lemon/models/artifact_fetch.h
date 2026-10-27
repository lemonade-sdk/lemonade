#pragma once

#include <lemon/models/download_progress.h>
#include <lemon/models/registry_id.h>
#include <lemon/utils/cancel_token.h>

namespace lemon {

struct ModelInfo;

namespace models {

// The two things a backend needs while it fetches: which registry a model's bytes
// come from, and the engine that writes them into the models directory. Handing
// over this instead of the whole model layer keeps backend policy from reaching
// into registry, cache and store state.
class ArtifactFetchService {
  public:
    virtual ~ArtifactFetchService() = default;

    // The registry a pull of this model reads: an explicit request wins over the
    // registry the model was registered with.
    virtual RegistryId download_source_for(const ModelInfo& info) const = 0;

    // The shared registry download: space check, transfer, verify, progress.
    virtual void fetch_registry_artifacts(const ModelInfo& info,
                                          DownloadProgressCallback progress,
                                          const utils::CancelToken& cancel) = 0;
};

}  // namespace models
}  // namespace lemon
