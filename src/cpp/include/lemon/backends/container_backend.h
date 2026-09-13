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
    // backends with their own requirements (Halogen) override it.
    virtual std::string profile_id(const std::string& variant) const;

protected:
    std::string recipe_;
};

// The device profile a variant needs, using the default name-based mapping.
std::string default_profile_id(const std::string& variant);

// URL of the container prerequisites page, optionally anchored at the section
// that fixes `remediation_id`.
std::string prerequisites_url(const std::string& remediation_id = "");

// ---------------------------------------------------------------------------
// Run side
// ---------------------------------------------------------------------------

struct ContainerLaunchRequest {
    std::string recipe;
    std::string variant;
    std::string profile_id;  // "" = derive from the variant name
    // Extra host directories to bind-mount read-only at the same path inside the
    // container. The Hugging Face cache is mounted automatically.
    std::vector<std::string> extra_mounts;
    std::vector<std::pair<std::string, std::string>> env;
    std::string entrypoint;  // "" = the image's own entrypoint
    std::string workdir;
};

// A resolved launch: which engine, which image, how the container is wired, and
// the argv to hand ProcessManager once the workload command is filled in.
class ContainerLaunchPlan {
public:
    // Host path -> the path the container sees. Throws when no mount covers it,
    // because launching with an unreachable model path fails opaquely inside the
    // container instead.
    std::string container_path(const std::string& host_path) const;

    void set_command(std::vector<std::string> command) {
        spec_.command = std::move(command);
    }

    // Environment for the container. Backends configured entirely through
    // environment variables (Halogen) fill this in after resolving paths
    // through container_path().
    void add_env(const std::string& key, const std::string& value) {
        spec_.env.push_back({key, value});
    }

    // argv for ProcessManager::start_process(engine_executable(), args).
    std::vector<std::string> engine_args() const;
    const std::string& engine_executable() const { return engine_.executable; }
    const std::string& container_name() const { return spec_.name; }
    int host_port() const { return spec_.host_port; }
    const utils::ContainerImageRef& image() const { return image_; }

private:
    friend ContainerLaunchPlan plan_container_launch(const ContainerLaunchRequest&, int);
    utils::ContainerEngine engine_;
    utils::ContainerRunSpec spec_;
    utils::ContainerImageRef image_;
};

// Build the plan for a launch. Throws std::runtime_error carrying the
// remediation text when the host cannot run containers, when no image is pinned
// for this host, or when the image has not been pulled.
ContainerLaunchPlan plan_container_launch(const ContainerLaunchRequest& request, int host_port);

// Remove any container left over from a previous run of this (recipe, variant)
// so a relaunch is not refused for a name collision.
void clear_stale_container(const std::string& recipe, const std::string& variant);

// Stop the container by name. Call this from unload() BEFORE killing the engine
// client process: signalling the client orphans the container, which keeps
// holding the GPU.
void stop_container_for(const std::string& recipe, const std::string& variant);

// ContainerRuntime::sweep_containers() over everything Lemonade manages.
int sweep_managed_containers();

}  // namespace backends
}  // namespace lemon
