#include "lemon/backends/container_backend.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/container_image_pins.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;

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

namespace {

constexpr const char* kModelsMountRoot = "/mnt/models";

// The mount the engine on the host needs in order to expose `visible_path`
// (a path as lemond sees it) at `inside`. Native, toolbox and snap transports
// share the host filesystem (a toolbox exposes the host under /run/host, which
// is the same tree). Inside a container the path is looked up through lemond's
// own mounts, which may be a named volume rather than a host directory.
utils::ContainerMount host_mount_for(utils::ContainerRuntime& runtime,
                                     const std::string& visible_path,
                                     const std::string& inside) {
    utils::ContainerMount mount;
    mount.container_path = inside;
    mount.read_only = true;
    switch (runtime.transport()) {
        case utils::HostTransport::Native:
        case utils::HostTransport::Snap:
            mount.host_path = visible_path;
            return mount;
        case utils::HostTransport::Toolbox: {
            const std::string kRunHost = "/run/host";
            mount.host_path = visible_path.rfind(kRunHost + "/", 0) == 0
                                  ? visible_path.substr(kRunHost.size())
                                  : visible_path;
            return mount;
        }
        case utils::HostTransport::Container: {
            // Longest matching self-mount wins; the container path of that mount
            // is the prefix to strip and its host source the prefix to add.
            const utils::ContainerMount* best = nullptr;
            for (const auto& self_mount : runtime.self_mounts()) {
                const std::string& prefix = self_mount.container_path;
                if (visible_path.rfind(prefix, 0) != 0) continue;
                if (visible_path.size() > prefix.size() && visible_path[prefix.size()] != '/' &&
                    prefix.back() != '/') {
                    continue;
                }
                if (!best || prefix.size() > best->container_path.size()) best = &self_mount;
            }
            if (!best) {
                throw std::runtime_error(
                    "'" + visible_path +
                    "' is not inside any volume mounted into lemond's container, so the "
                    "engine on the host cannot mount it. See " +
                    prerequisites_url("engine-socket"));
            }
            std::string suffix = visible_path.substr(best->container_path.size());
            if (!suffix.empty() && suffix.front() == '/') suffix.erase(0, 1);
            if (best->volume) {
                mount.volume = true;
                mount.host_path = best->host_path;
                mount.volume_subpath = suffix;
            } else {
                mount.host_path =
                    suffix.empty() ? best->host_path : best->host_path + "/" + suffix;
            }
            return mount;
        }
    }
    mount.host_path = visible_path;
    return mount;
}

}  // namespace

std::string ContainerLaunchPlan::container_path(const std::string& host_path) const {
    std::error_code ec;
    const fs::path canonical = fs::canonical(host_path, ec);
    const std::string resolved = ec ? host_path : canonical.string();
    const std::string mapped = ContainerRuntime::rewrite_path(visible_mounts_, resolved);
    if (mapped.empty()) {
        throw std::runtime_error("Path '" + host_path +
                                 "' is outside every directory mounted into the container");
    }
    return mapped;
}

std::vector<std::string> ContainerLaunchPlan::engine_args() const {
    std::vector<std::string> args = engine_prefix_;
    const std::vector<std::string> run = ContainerRuntime::build_run_args(engine_, spec_);
    args.insert(args.end(), run.begin(), run.end());
    return args;
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
    std::tie(plan.engine_executable_, plan.engine_prefix_) = runtime.engine_invocation(*engine);
    plan.image_ = pin_or_throw(request.recipe, request.variant);

    if (!runtime.has_image_digest(plan.image_.repository, plan.image_.digest)) {
        throw std::runtime_error("Toolbox image " + plan.image_.tagged_ref() +
                                 " is not installed at the pinned digest");
    }

    plan.spec_.name = ContainerRuntime::container_name(request.recipe, request.variant);
    plan.spec_.image = plan.image_.pinned_ref();
    plan.spec_.labels = {
        {std::string(ContainerRuntime::managed_label()) + ".recipe", request.recipe},
        {std::string(ContainerRuntime::managed_label()) + ".variant", request.variant},
        {std::string(ContainerRuntime::managed_label()) + ".port", std::to_string(host_port)},
    };
    plan.spec_.profile =
        ContainerRuntime::resolve_profile_groups(ContainerRuntime::device_profile(profile));
    plan.spec_.host_port = host_port;
    plan.spec_.container_port = host_port;
    plan.spec_.env = request.env;
    plan.spec_.entrypoint = request.entrypoint;
    plan.spec_.workdir = request.workdir;

    // Inside a container lemond's loopback is its own network namespace, so the
    // workload joins it instead of publishing a port to the host. Everywhere
    // else the workload gets a private internal network of its own: no default
    // route, no NAT, nothing else on it. Docker cannot publish a port from such
    // a network but the host can route to the container's address on it;
    // rootless podman is the reverse, so it publishes on loopback.
    const std::string self_id = runtime.self_container_id();
    if (!self_id.empty()) {
        plan.spec_.network = "container:" + self_id;
        plan.spec_.publish_port = false;
    } else {
        runtime.ensure_isolated_network(plan.spec_.name);
        plan.spec_.network = plan.spec_.name;
        plan.spec_.publish_port = (engine->kind == utils::ContainerEngineKind::Podman);
    }

    // sysfs shows the host's KFD topology in every transport, so the GPU index
    // is computed the same way everywhere; no match simply adds nothing.
    const std::string gpu_index =
        ContainerRuntime::kfd_gpu_index_for_arch(SystemInfo::get_rocm_arch());
    if (!gpu_index.empty()) plan.spec_.env.push_back({"HIP_VISIBLE_DEVICES", gpu_index});

    // Each model file is resolved through the Hugging Face cache's symlinks and
    // mounted alone under /mnt/models, so the container sees exactly the files
    // it was given and the in-container path is the same on every host.
    for (const auto& model_path : request.model_paths) {
        if (model_path.empty()) continue;
        std::error_code ec;
        const fs::path canonical = fs::canonical(model_path, ec);
        if (ec) {
            throw std::runtime_error("Model path '" + model_path + "' does not exist");
        }
        const std::string visible = canonical.string();
        if (!ContainerRuntime::rewrite_path(plan.visible_mounts_, visible).empty()) continue;
        const std::string inside =
            std::string(kModelsMountRoot) + "/" + canonical.filename().string();
        utils::ContainerMount visible_mount;
        visible_mount.host_path = visible;
        visible_mount.container_path = inside;
        plan.visible_mounts_.push_back(visible_mount);
        plan.spec_.mounts.push_back(host_mount_for(runtime, visible, inside));
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
    const std::string name = ContainerRuntime::container_name(recipe, variant);
    runtime.stop_container(name);
    // --rm detaches the container from its network asynchronously, so remove it
    // explicitly first or the network is still in use.
    runtime.remove_container(name);
    runtime.remove_network(name);
}

std::string wait_for_container_address(const std::string& recipe, const std::string& variant,
                                       int timeout_seconds) {
    auto& runtime = ContainerRuntime::global();
    if (!runtime.engine()) return "";
    const std::string name = ContainerRuntime::container_name(recipe, variant);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string address = runtime.container_address(name);
        if (!address.empty()) return address;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return "";
}

std::string container_logs_for(const std::string& recipe, const std::string& variant) {
    auto& runtime = ContainerRuntime::global();
    if (!runtime.engine()) return "";
    return runtime.container_logs(ContainerRuntime::container_name(recipe, variant));
}

int sweep_managed_containers() {
    auto& runtime = ContainerRuntime::global();
    if (!runtime.engine()) return 0;
    const int removed = runtime.sweep_managed_containers();
    if (removed > 0) {
        LOG(INFO, "Container") << "Swept " << removed << " stale Lemonade container(s)"
                               << std::endl;
    }
    return removed;
}

}  // namespace backends
}  // namespace lemon
