#include "lemon/backends/container_backend.h"

#include <stdexcept>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/container_image_pins.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

namespace lemon {
namespace backends {

using utils::ContainerRuntime;

namespace {

constexpr const char* kPrerequisitesUrl = "https://lemonade-server.ai/container_prerequisites.html";

utils::ContainerImageRef pin_or_throw(const std::string& recipe, const std::string& variant) {
    const utils::ContainerImageRef ref = image_pin(recipe, variant);
    if (!ref.valid()) {
        const std::string arch = SystemInfo::get_rocm_arch();
        throw std::runtime_error(recipe + ":" + variant + " publishes no toolbox image for " +
                                 (arch.empty() ? std::string("this GPU") : arch));
    }
    return ref;
}

}  // namespace

std::string prerequisites_url(const std::string& remediation_id) {
    return remediation_id.empty() ? std::string(kPrerequisitesUrl)
                                  : std::string(kPrerequisitesUrl) + "#" + remediation_id;
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
        state.action = prerequisites_url(readiness.remediation_id);
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
    const utils::ContainerImageRef ref = pin_or_throw(recipe_, backend);
    auto& runtime = ContainerRuntime::global();

    const auto readiness =
        runtime.check_readiness(ContainerRuntime::device_profile(profile_id(backend)));
    if (readiness.state == utils::ContainerReadiness::NoEngine ||
        readiness.state == utils::ContainerReadiness::EngineUnreachable) {
        throw std::runtime_error(readiness.message + " See " +
                                 prerequisites_url(readiness.remediation_id));
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

// ---------------------------------------------------------------------------
// Run side
// ---------------------------------------------------------------------------

std::string ContainerLaunchPlan::container_path(const std::string& host_path) const {
    const std::string mapped = ContainerRuntime::rewrite_path(spec_.mounts, host_path);
    if (mapped.empty()) {
        throw std::runtime_error("Path '" + host_path +
                                 "' is outside every directory mounted into the container");
    }
    return mapped;
}

std::vector<std::string> ContainerLaunchPlan::engine_args() const {
    return ContainerRuntime::build_run_args(engine_, spec_);
}

ContainerLaunchPlan plan_container_launch(const ContainerLaunchRequest& request, int host_port) {
    auto& runtime = ContainerRuntime::global();

    ContainerLaunchPlan plan;

    // Host readiness first: a machine with no container runtime should say so,
    // not report a missing image pin it could not have used anyway.
    const std::string profile = request.profile_id.empty() ? default_profile_id(request.variant)
                                                           : request.profile_id;
    const auto readiness = runtime.check_readiness(ContainerRuntime::device_profile(profile));
    if (!readiness.ok()) {
        throw std::runtime_error(readiness.message + " See " +
                                 prerequisites_url(readiness.remediation_id));
    }

    const auto& engine = runtime.engine();
    if (!engine) {
        throw std::runtime_error("No container runtime found. See " + prerequisites_url(
                                     "no-container-runtime"));
    }
    plan.engine_ = *engine;
    plan.image_ = pin_or_throw(request.recipe, request.variant);

    if (!runtime.has_image_digest(plan.image_.repository, plan.image_.digest)) {
        throw std::runtime_error("Toolbox image " + plan.image_.tagged_ref() +
                                 " is not installed at the pinned digest");
    }

    plan.spec_.name = ContainerRuntime::container_name(request.recipe, request.variant);
    plan.spec_.image = plan.image_.pinned_ref();
    plan.spec_.profile =
        ContainerRuntime::resolve_profile_groups(ContainerRuntime::device_profile(profile));
    plan.spec_.host_port = host_port;
    plan.spec_.container_port = host_port;
    plan.spec_.env = request.env;
    plan.spec_.entrypoint = request.entrypoint;
    plan.spec_.workdir = request.workdir;

    // The Hugging Face cache is a tree of symlinks into blobs/, so mounting the
    // resolved model file alone lands a dangling link inside the container. Mount
    // the cache root read-only at the same path and rewrite model arguments
    // through it.
    const std::string hf_cache = utils::get_hf_cache_dir();
    if (!hf_cache.empty()) {
        plan.spec_.mounts.push_back({hf_cache, hf_cache, /*read_only=*/true});
    }
    // A model resolved out of the Hugging Face cache is already covered by the
    // mount above. Adding it again would nest one bind inside another for no
    // gain; extra mounts exist for models registered by an absolute path
    // somewhere else.
    for (const auto& extra : request.extra_mounts) {
        if (extra.empty()) continue;
        if (!ContainerRuntime::rewrite_path(plan.spec_.mounts, extra).empty()) continue;
        plan.spec_.mounts.push_back({extra, extra, /*read_only=*/true});
    }

    return plan;
}

void clear_stale_container(const std::string& recipe, const std::string& variant) {
    auto& runtime = ContainerRuntime::global();
    if (!runtime.engine()) return;
    runtime.remove_container(ContainerRuntime::container_name(recipe, variant));
}

void stop_container_for(const std::string& recipe, const std::string& variant) {
    auto& runtime = ContainerRuntime::global();
    if (!runtime.engine()) return;
    runtime.stop_container(ContainerRuntime::container_name(recipe, variant));
}

int sweep_managed_containers() {
    auto& runtime = ContainerRuntime::global();
    if (!runtime.engine()) return 0;
    const int removed = runtime.sweep_containers(ContainerRuntime::managed_name_prefix());
    if (removed > 0) {
        LOG(INFO, "Container") << "Swept " << removed << " stale Lemonade container(s)"
                               << std::endl;
    }
    return removed;
}

}  // namespace backends
}  // namespace lemon
