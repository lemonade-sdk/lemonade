#include "lemon/backends/container_backend.h"

#include <stdexcept>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/container_image_pins.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"

namespace lemon {
namespace backends {

using utils::ContainerRuntime;

utils::ContainerImageRef pinned_image_or_throw(const std::string& recipe,
                                               const std::string& variant) {
    const utils::ContainerImageRef ref = image_pin(recipe, variant);
    if (!ref.valid()) {
        const std::string arch = SystemInfo::get_rocm_arch();
        throw std::runtime_error(recipe + ":" + variant + " publishes no toolbox image for " +
                                 (arch.empty() ? std::string("this GPU") : arch));
    }
    return ref;
}

std::string default_profile_id(const std::string& variant) {
    if (variant.rfind("vulkan", 0) == 0) return "vulkan";
    return "amd-rocm";
}

std::string ContainerBackendOps::profile_id(const std::string& variant) const {
    return default_profile_id(variant);
}

std::string ContainerBackendOps::artifact_url(const std::string& backend) const {
    return registry_url(image_pin(recipe_, backend));
}

BackendOps::InstallCheck ContainerBackendOps::check_install(const std::string& backend,
                                                            bool binary_found) const {
    (void)binary_found;  // there is no managed binary on disk for an image backend

    const utils::ContainerImageRef ref = image_pin(recipe_, backend);
    if (!ref.valid()) {
        const std::string arch = SystemInfo::get_rocm_arch();
        return {false, "No toolbox image is published for " +
                           (arch.empty() ? std::string("this GPU") : arch)};
    }

    auto& runtime = ContainerRuntime::global();
    const auto readiness = runtime.check_readiness(ContainerRuntime::device_profile(
        profile_id(backend)));
    if (!readiness.ok()) {
        return {false, readiness.message};
    }

    if (!runtime.has_image_digest(ref.repository, ref.digest)) {
        return {false, "Toolbox image " + ref.tagged_ref() + " has not been pulled."};
    }
    return {true, ""};
}

std::string ContainerBackendOps::resolve_version(const std::string& backend,
                                                 const std::string& file_version) const {
    (void)file_version;  // image backends keep no version.txt
    const utils::ContainerImageRef ref = image_pin(recipe_, backend);
    if (!ref.valid()) return "";
    auto& runtime = ContainerRuntime::global();
    // Variants of one recipe are usually tags of the same repository, so report
    // this variant's own pinned digest when it is present. Falling back to
    // whatever else that repository has locally is what makes an out-of-date
    // pull read as update_required rather than as installed.
    if (runtime.has_image_digest(ref.repository, ref.digest)) return short_digest(ref.digest);
    return short_digest(runtime.installed_digest(ref.repository));
}

std::optional<BackendOps::UnavailableState> ContainerBackendOps::classify_unavailable(
    const std::string& backend, const std::string& install_error,
    const std::string& default_install_command) const {
    auto& runtime = ContainerRuntime::global();
    const auto readiness =
        runtime.check_readiness(ContainerRuntime::device_profile(profile_id(backend)));
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
    const utils::ContainerImageRef ref = pinned_image_or_throw(recipe_, backend);
    auto& runtime = ContainerRuntime::global();

    const auto readiness =
        runtime.check_readiness(ContainerRuntime::device_profile(profile_id(backend)));
    if (readiness.state == utils::ContainerReadiness::NoEngine ||
        readiness.state == utils::ContainerReadiness::EngineUnreachable) {
        throw std::runtime_error(readiness.message + " See " +
                                 utils::container_prerequisites_url(readiness.remediation_id));
    }

    if (!force && runtime.has_image_digest(ref.repository, ref.digest)) {
        if (progress) {
            DownloadProgress p;
            p.file = ref.tagged_ref();
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
            throw std::runtime_error("Cannot pull " + ref.tagged_ref() + ": " +
                                     (cfg->offline() ? "offline mode"
                                                     : "fetching executable artifacts is disabled"));
        }
    }

    runtime.pull(ref, progress);
    return true;
}

bool ContainerBackendOps::uninstall(const std::string& backend) const {
    const utils::ContainerImageRef ref = image_pin(recipe_, backend);
    if (!ref.valid()) return true;  // nothing pinned for this host; nothing to remove
    ContainerRuntime::global().remove_image(ref);
    return true;
}

}  // namespace backends
}  // namespace lemon
