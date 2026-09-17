#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lemon/utils/process_manager.h"

namespace lemon {
struct DownloadProgress;
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;
}

namespace lemon {
namespace utils {

// Which OCI engine is driving the containers. Podman is preferred; see
// ContainerRunSpec::to_argv() for what that buys.
enum class ContainerEngineKind { None, Podman, Docker };

// Where lemond itself is running relative to the engine. Everything but Native
// means the engine lives outside lemond's own filesystem and process namespace,
// so engine calls, mount sources and port reachability are translated.
enum class HostTransport {
    Native,     // engine binary on PATH, same namespace as lemond
    Toolbox,    // Fedora Toolbox / Distrobox: engine runs on the host via flatpak-spawn
    Container,  // lemond is itself a container talking to a bind-mounted engine socket
    Snap,       // strictly confined snap driving the Docker snap's daemon
};

const char* host_transport_name(HostTransport transport);

// URL of the container prerequisites page, optionally anchored at the section
// that fixes a ReadinessResult's `remediation_id`.
std::string container_prerequisites_url(const std::string& remediation_id = "");

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
// entry rather than a branch in the launch path. Anything here beyond devices
// and groups loosens the default confinement, so each such entry records why.
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
    std::string host_path;  // a host directory or file, or a volume name when `volume`
    std::string container_path;
    bool read_only = true;
    // Set when host_path names an engine volume rather than a host path. The
    // engine then mounts `volume_subpath` inside that volume; "" is the whole
    // volume. Docker calls this volume-subpath, podman subpath.
    bool volume = false;
    std::string volume_subpath;
};

struct ContainerRunSpec {
    std::string name;
    std::string image;  // pinned "<repository>@sha256:..." reference
    std::vector<std::pair<std::string, std::string>> labels;
    DeviceProfile profile;
    std::vector<ContainerMount> mounts;
    std::vector<std::pair<std::string, std::string>> env;
    // A network name or "container:<id>"; "none" is unreachable, so start()
    // always overrides it.
    std::string network = "none";
    // Engine-dependent; start() explains the choice.
    bool publish_port = true;
    int host_port = 0;
    int container_port = 0;
    std::string entrypoint;          // "" = the image's own entrypoint
    std::vector<std::string> command;  // argv appended after the image
    std::string workdir;

    // The engine argv for this spec. Pure; the engine decides only where podman
    // and docker spell the same thing differently.
    std::vector<std::string> to_argv(const ContainerEngine& engine) const;
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

// What a backend asks to run inside a pinned image.
struct ContainerWorkload {
    std::string name;
    std::string recipe;
    std::string variant;
    ContainerImageRef image;
    std::string profile_id;
    // Bind-mounted read-only under /mnt/models. Any `command` or `env` entry
    // equal to one of these is rewritten to the path inside, so a caller never
    // spells an in-container path itself.
    std::vector<std::string> model_paths;
    std::vector<std::string> command;  // argv inside the image, argv[0] included
    std::vector<std::pair<std::string, std::string>> env;
    std::string entrypoint;  // "" = the image's own entrypoint
    std::string workdir;
    int port = 0;
    bool inherit_output = false;
};

struct RunningChild {
    ProcessHandle client{nullptr, 0};
    std::string executable;
    std::vector<std::string> args;
    std::string host;  // already resolved, published port or container address
    int port = 0;
    // Empty on the host. A container is not lemond's child, so stopping one
    // takes this name rather than `client`.
    std::string container;
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
    NoHostMountMapping,
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
    explicit ContainerRuntime(CommandRunner runner = nullptr,
                              std::optional<HostTransport> transport = std::nullopt);

    // Process-wide instance used by the backends.
    static ContainerRuntime& global();

    // --- host transport ---------------------------------------------------
    HostTransport transport() const { return transport_; }
    static HostTransport detect_transport();
    // The engine executable and leading argv for this transport. In a toolbox
    // that is `flatpak-spawn --host <engine>`; a podman reached over a socket
    // gets `--remote`.
    std::pair<std::string, std::vector<std::string>> engine_invocation(
        const ContainerEngine& engine) const;
    // Container transport: the id of the container lemond runs in, from the
    // engine's own view, or "" when it cannot be determined.
    std::string self_container_id();
    // Container transport: lemond's own mounts as (host source -> path inside
    // lemond's container). A path lemond sees is translated back to the host
    // through these before it is handed to the engine as a mount source.
    const std::vector<ContainerMount>& self_mounts();

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
    // Throws std::runtime_error carrying remediation text when the host cannot
    // run the workload.
    RunningChild start(const ContainerWorkload& workload);
    // Call BEFORE killing the engine client: SIGKILL is not proxied inward, and
    // the container would go on holding the GPU.
    void stop(const std::string& name);

    void stop_container(const std::string& name, int timeout_seconds = 10);
    void remove_container(const std::string& name);
    // The last `tail` lines the container wrote, for attaching to a failed load.
    std::string container_logs(const std::string& name, int tail = 60);
    // The container's address on its network, or "" while it has none yet.
    std::string container_address(const std::string& name);

    // Idempotent; `--internal`, carrying the managed label.
    void ensure_isolated_network(const std::string& name);
    void remove_network(const std::string& name);
    std::vector<std::string> list_managed_networks();
    // Every container carrying the managed label, running or not.
    std::vector<std::string> list_managed_containers();
    // Remove every container and private network carrying the managed label.
    // Called at startup so a killed lemond does not leave GPU-holding
    // containers behind.
    int sweep_managed_containers();

    // --- pure helpers (no engine needed; unit-tested directly) -------------
    static std::vector<std::string> build_pull_args(const ContainerImageRef& ref);
    static std::vector<std::string> build_stop_args(const std::string& name, int timeout_seconds);

    // Translate a host path into the path the container sees, using the longest
    // matching mount. Returns "" when no mount covers it.
    static std::string rewrite_path(const std::vector<ContainerMount>& mounts,
                                    const std::string& host_path);

    // Container name for a recipe/variant, for humans reading `ps`. Ownership is
    // the label, not the name.
    static std::string container_name(const std::string& recipe, const std::string& variant);
    // The bare label every managed container carries; `ps --filter label=` on
    // it is how sweep and listing find them.
    static const char* managed_label();

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

    // --- GPU selection -----------------------------------------------------
    // "gfx1151" for KFD's gfx_target_version 110501: major, then minor and
    // stepping as single hex digits.
    static std::string gfx_name_from_target_version(int target_version);
    // Index among the GPU nodes (target version != 0), in KFD node order, of
    // the first one whose name is `arch`; "" when none matches. That index is
    // what HIP_VISIBLE_DEVICES counts.
    static std::string pick_gpu_index(const std::vector<int>& target_versions,
                                      const std::string& arch);
    // The same, read from /sys/devices/virtual/kfd/kfd/topology/nodes.
    static std::string kfd_gpu_index_for_arch(const std::string& arch);

    // Named volumes have no usable host path from inside the sandbox, so their
    // source is the volume name and the engine resolves it.
    static std::vector<ContainerMount> parse_self_mounts(const std::string& inspect_json);

private:
    CommandResult run(const std::vector<std::string>& args, int timeout_seconds);
    CommandResult invoke(const ContainerEngine& engine, const std::vector<std::string>& args,
                         int timeout_seconds);
    std::optional<ContainerEngine> probe(ContainerEngineKind kind, const std::string& binary);

    CommandRunner runner_;
    HostTransport transport_;
    std::optional<ContainerEngine> engine_;
    bool engine_probed_ = false;
    bool self_probed_ = false;
    std::string self_id_;
    std::vector<ContainerMount> self_mounts_;
};

}  // namespace utils
}  // namespace lemon
