#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lemon/backends/backend_ops.h"
#include "lemon/utils/container_runtime.h"

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

    // Device passthrough profile for a variant. The default maps the variant
    // name onto the catalog profiles (rocm* -> amd-rocm, vulkan* -> vulkan);
    // a backend with its own requirements overrides it.
    virtual std::string profile_id(const std::string& variant) const;

protected:
    std::string recipe_;
};

// The device profile a variant needs, using the default name-based mapping.
std::string default_profile_id(const std::string& variant);

// ---------------------------------------------------------------------------
// Run side
// ---------------------------------------------------------------------------

// The pinned image for (recipe, variant) on this host. Throws when the recipe
// publishes nothing for this GPU, which is the only answer a caller can act on.
utils::ContainerImageRef pinned_image_or_throw(const std::string& recipe,
                                               const std::string& variant);

}  // namespace backends
}  // namespace lemon
