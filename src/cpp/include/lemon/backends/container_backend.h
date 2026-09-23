#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lemon/backends/backend_ops.h"
#include "lemon/utils/container_manager.h"

namespace lemon {
namespace backends {

// ---------------------------------------------------------------------------
// Install side
// ---------------------------------------------------------------------------

// BackendOps for a recipe that runs inside an OCI image. Derive from it and
// pass the recipe name.
class ContainerBackendOps : public BackendOps {
public:
    explicit ContainerBackendOps(std::string recipe) : recipe_(std::move(recipe)) {}

    bool install(const std::string& backend, bool force,
                 DownloadProgressCallback progress) const override;
    bool uninstall(const std::string& backend) const override;
    std::string resolve_version(const std::string& backend,
                                const std::string& file_version) const override;
    InstallCheck check_install(const std::string& backend, bool binary_found) const override;
    std::optional<UnavailableState> classify_unavailable(
        const std::string& backend, const std::string& install_error,
        const std::string& default_install_command) const override;
    std::string artifact_url(const std::string& backend) const override;

protected:
    std::string recipe_;
};

// ---------------------------------------------------------------------------
// Run side
// ---------------------------------------------------------------------------

// image_pin() for this host, throwing instead when the result cannot run here:
// nothing is published for this GPU, a listed device is unusable, or the digest
// has not been pulled.
utils::ContainerImage pinned_image_or_throw(const std::string& recipe,
                                            const std::string& backend);

}  // namespace backends
}  // namespace lemon
