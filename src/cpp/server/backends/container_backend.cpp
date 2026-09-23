#include "lemon/backends/container_backend.h"

#include <stdexcept>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/container_image_pins.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"

namespace lemon {
namespace backends {

using utils::ContainerManager;

namespace {

utils::ContainerImage published_image_or_throw(const std::string& recipe,
                                               const std::string& backend) {
    utils::ContainerImage image = image_pin(recipe, backend);
    if (!image.valid()) {
        const std::string arch = SystemInfo::get_rocm_arch();
        throw std::runtime_error(recipe + ":" + backend + " publishes no toolbox image for " +
                                 (arch.empty() ? std::string("this GPU") : arch));
    }
    return image;
}

// Why this host cannot run `image` right now, or "" when it can.
std::string run_problem(const utils::ContainerImage& image) {
    auto& manager = ContainerManager::global();
    const auto readiness = manager.check_readiness(image.devices);
    if (!readiness.ok()) {
        return readiness.message + " See " +
               utils::container_prerequisites_url(readiness.remediation_id);
    }
    if (!manager.has_image_digest(image.repository, image.digest)) {
        return "Toolbox image " + image.tagged_ref() + " has not been pulled.";
    }
    return "";
}

}  // namespace

utils::ContainerImage pinned_image_or_throw(const std::string& recipe,
                                            const std::string& backend) {
    utils::ContainerImage image = published_image_or_throw(recipe, backend);
    if (const std::string problem = run_problem(image); !problem.empty()) {
        throw std::runtime_error(problem);
    }
    return image;
}

std::string ContainerBackendOps::artifact_url(const std::string& backend) const {
    return registry_url(image_pin(recipe_, backend));
}

BackendOps::InstallCheck ContainerBackendOps::check_install(const std::string& backend,
                                                            bool binary_found) const {
    (void)binary_found;  // a container backend has no binary on disk

    const utils::ContainerImage image = image_pin(recipe_, backend);
    if (!image.valid()) {
        const std::string arch = SystemInfo::get_rocm_arch();
        return {false, "No toolbox image is published for " +
                           (arch.empty() ? std::string("this GPU") : arch)};
    }

    const std::string problem = run_problem(image);
    return {problem.empty(), problem};
}

std::string ContainerBackendOps::resolve_version(const std::string& backend,
                                                 const std::string& file_version) const {
    (void)file_version;  // a container backend keeps no version.txt
    const utils::ContainerImage image = image_pin(recipe_, backend);
    if (!image.valid()) return "";
    auto& manager = ContainerManager::global();
    // Backends of one recipe are usually tags of the same repository, so report
    // this backend's own pinned digest when it is present. Falling back to
    // whatever else that repository has locally is what makes an out-of-date
    // pull read as update_required rather than as installed.
    if (manager.has_image_digest(image.repository, image.digest)) return short_digest(image.digest);
    return short_digest(manager.installed_digest(image.repository));
}

std::optional<BackendOps::UnavailableState> ContainerBackendOps::classify_unavailable(
    const std::string& backend, const std::string& install_error,
    const std::string& default_install_command) const {
    const auto readiness =
        ContainerManager::global().check_readiness(image_pin(recipe_, backend).devices);
    if (!readiness.ok()) {
        UnavailableState state;
        state.state = "action_required";
        state.message = readiness.message;
        state.action = utils::container_prerequisites_url(readiness.remediation_id);
        return state;
    }

    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->no_fetch_executables()) {
            UnavailableState state;
            state.state = "unsupported";
            state.message = "Automatic backend install is disabled.";
            state.action = "";
            return state;
        }
    }

    UnavailableState state;
    state.state = "installable";
    state.message = install_error.empty() ? "Toolbox image has not been pulled." : install_error;
    state.action = default_install_command;
    return state;
}

bool ContainerBackendOps::install(const std::string& backend, bool force,
                                  DownloadProgressCallback progress) const {
    const utils::ContainerImage image = published_image_or_throw(recipe_, backend);
    auto& manager = ContainerManager::global();

    const auto readiness = manager.check_readiness(image.devices);
    if (readiness.state == utils::ContainerReadiness::NoContainerTool ||
        readiness.state == utils::ContainerReadiness::ContainerToolUnreachable) {
        throw std::runtime_error(readiness.message + " See " +
                                 utils::container_prerequisites_url(readiness.remediation_id));
    }

    if (!force && manager.has_image_digest(image.repository, image.digest)) {
        if (progress) {
            DownloadProgress p;
            p.file = image.tagged_ref();
            p.file_index = 1;
            p.total_files = 1;
            p.percent = 100;
            p.complete = true;
            progress(p);
        }
        return true;
    }

    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->offline() || cfg->no_fetch_executables()) {
            throw std::runtime_error("Cannot pull " + image.tagged_ref() + ": " +
                                     (cfg->offline() ? "offline mode"
                                                     : "fetching executable artifacts is disabled"));
        }
    }

    manager.pull(image, progress);
    return true;
}

bool ContainerBackendOps::uninstall(const std::string& backend) const {
    const utils::ContainerImage image = image_pin(recipe_, backend);
    if (!image.valid()) return true;  // nothing pinned for this host; nothing to remove
    ContainerManager::global().remove_image(image);
    return true;
}

}  // namespace backends
}  // namespace lemon
