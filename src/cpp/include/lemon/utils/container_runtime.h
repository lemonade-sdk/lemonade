#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
struct DownloadProgress;
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;
}

namespace lemon {
namespace utils {

// Which OCI engine is driving the containers. Podman is preferred; see
// build_run_args() for what that buys.
enum class ContainerEngineKind { None, Podman, Docker };

struct ContainerEngine {
    ContainerEngineKind kind = ContainerEngineKind::None;
    std::string executable;  // absolute path, as resolved on PATH
    std::string version;     // engine version string, "" when not probed
    bool rootless = false;

    std::string name() const {
        switch (kind) {
            case ContainerEngineKind::Podman: return "podman";
            case ContainerEngineKind::Docker: return "docker";
            case ContainerEngineKind::None:   return "";
        }
        return "";
    }
    bool valid() const { return kind != ContainerEngineKind::None; }
};

// Device passthrough for one class of workload, mirroring the runtime_profiles
// in the upstream toolbox catalog. Held as data so a new profile is a table
// entry rather than a branch in the launch path.
struct DeviceProfile {
    std::string id;
    std::vector<std::string> devices;                          // --device
    std::vector<std::string> groups;                           // --group-add
    std::vector<std::string> security_opts;                    // --security-opt
    std::vector<std::pair<std::string, std::string>> env;      // --env
    std::vector<std::string> cap_add;                          // --cap-add
    bool ipc_host = false;                                     // --ipc=host
    bool memlock_unlimited = false;                            // --ulimit memlock=-1:-1
};

struct ContainerMount {
    std::string host_path;
    std::string container_path;
    bool read_only = true;
};

struct ContainerRunSpec {
    std::string name;
    std::string image;  // pinned "<repository>@sha256:..." reference
    DeviceProfile profile;
    std::vector<ContainerMount> mounts;
    std::vector<std::pair<std::string, std::string>> env;
    int host_port = 0;
    int container_port = 0;
    std::string entrypoint;          // "" = the image's own entrypoint
    std::vector<std::string> command;  // argv appended after the image
    std::string workdir;
};

// A pinned image. `tag` is documentation only: `digest` is what gets pulled, so
// an upstream retag cannot change what a release of Lemonade runs.
struct ContainerImageRef {
    std::string repository;
    std::string tag;
    std::string digest;
    std::string channel;  // "stable" | "experimental"

    bool valid() const { return !repository.empty(); }
    std::string pinned_ref() const {
        if (!digest.empty()) return repository + "@" + digest;
        if (!tag.empty()) return repository + ":" + tag;
        return repository;
    }
    std::string tagged_ref() const {
        return tag.empty() ? repository : repository + ":" + tag;
    }
};

// Why a container-backed recipe cannot run right now. Each value maps to a
// section id on the container prerequisites doc page, so /system-info can hand
// the user a link that lands on the fix for their specific problem.
enum class ContainerReadiness {
    Ready,
    NoEngine,
    EngineUnreachable,
    NoKfd,
    NoRenderNode,
    NoGroupMembership,
};

struct ReadinessResult {
    ContainerReadiness state = ContainerReadiness::Ready;
    std::string message;
    std::string remediation_id;  // anchor on the prerequisites page

    bool ok() const { return state == ContainerReadiness::Ready; }
};

struct CommandResult {
    int exit_code = -1;
    std::string output;  // stdout and stderr interleaved
};

// Engine invocation, injectable so the whole layer can be exercised against a
// fake engine binary without a container runtime installed.
using CommandRunner = std::function<CommandResult(const std::string& executable,
                                                  const std::vector<std::string>& args,
                                                  int timeout_seconds)>;

class ContainerRuntime {
public:
    explicit ContainerRuntime(CommandRunner runner = nullptr);

    // Process-wide instance used by the backends.
    static ContainerRuntime& global();

    // --- engine discovery -------------------------------------------------
    // Resolves podman first, then docker, and verifies the daemon/socket
    // actually answers. Cached; reset_engine_cache() re-probes.
    const std::optional<ContainerEngine>& engine();
    void reset_engine_cache();

    // Is the host able to run `profile`? Returns the first blocking problem.
    ReadinessResult check_readiness(const DeviceProfile& profile);

    // --- image operations -------------------------------------------------
    // Every locally present digest for `repository`. Several variants of one
    // recipe are usually different tags of the SAME repository, so all of them
    // show up here and a variant is identified by its own digest, never by
    // "the digest this repository has".
    std::vector<std::string> installed_digests(const std::string& repository);
    // True when `digest` is one of them.
    bool has_image_digest(const std::string& repository, const std::string& digest);
    // First locally present digest, or "" - the fallback for reporting what is
    // installed when it is not the pinned digest.
    std::string installed_digest(const std::string& repository);
    bool image_present(const ContainerImageRef& ref);
    void pull(const ContainerImageRef& ref, DownloadProgressCallback progress = nullptr);
    void remove_image(const ContainerImageRef& ref);

    // --- container lifecycle ----------------------------------------------
    void stop_container(const std::string& name, int timeout_seconds = 10);
    void remove_container(const std::string& name);
    std::vector<std::string> list_containers(const std::string& name_prefix);
    // Remove every container whose name starts with `name_prefix`. Called at
    // startup so a killed lemond does not leave GPU-holding containers behind.
    int sweep_containers(const std::string& name_prefix);

    // --- pure helpers (no engine needed; unit-tested directly) -------------
    static std::vector<std::string> build_run_args(const ContainerEngine& engine,
                                                   const ContainerRunSpec& spec);
    static std::vector<std::string> build_pull_args(const ContainerImageRef& ref);
    static std::vector<std::string> build_stop_args(const std::string& name, int timeout_seconds);

    // Translate a host path into the path the container sees, using the longest
    // matching mount. Returns "" when no mount covers it. The HF cache is a tree
    // of symlinks into blobs/, so it is bind-mounted whole and model paths are
    // rewritten through this rather than mounted file by file.
    static std::string rewrite_path(const std::vector<ContainerMount>& mounts,
                                    const std::string& host_path);

    // Container name for a recipe/variant. The shared prefix is what
    // sweep_containers() matches on, so every managed container must use this.
    static std::string container_name(const std::string& recipe, const std::string& variant);
    static const char* managed_name_prefix();

    // The named profile, or an empty profile when `id` is unknown.
    static const DeviceProfile& device_profile(const std::string& id);

    // A copy of `profile` with its group NAMES replaced by the host's numeric
    // gids. `--group-add <name>` is resolved by the engine against the
    // container's group file, not the host's, so a name is wrong twice over: the
    // image may not define it at all (the run is refused), and where it does the
    // gid may differ from the host's, which silently grants nothing. The gid is
    // what the device node actually checks. Names the host does not define are
    // dropped; podman's `keep-groups` keyword is passed through.
    static DeviceProfile resolve_profile_groups(const DeviceProfile& profile);
    // Host gid for a group name as a decimal string, or "" when undefined.
    static std::string host_group_gid(const std::string& name);
    static std::vector<std::string> device_profile_ids();

    // Parse `<engine> --version` output into a bare version string.
    static std::string parse_engine_version(const std::string& output);
    // Digests for `repository` in `<engine> images --digests --format ...`
    // output, whose lines are "<repository>@<digest>".
    static std::vector<std::string> parse_repo_digests(const std::string& output,
                                                       const std::string& repository);
    // The first of them, or "".
    static std::string parse_repo_digest(const std::string& output,
                                         const std::string& repository);

private:
    CommandResult run(const std::vector<std::string>& args, int timeout_seconds);
    std::optional<ContainerEngine> probe(ContainerEngineKind kind, const std::string& binary);

    CommandRunner runner_;
    std::optional<ContainerEngine> engine_;
    bool engine_probed_ = false;
};

}  // namespace utils
}  // namespace lemon
