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

enum class ContainerTool { None, Podman, Docker };

// Where lemond itself is running relative to podman or docker. Everything but
// Native means the container tool lives outside lemond's own filesystem and
// process namespace, so its calls, mount sources and port reachability are
// translated.
enum class HostTransport {
    Native,     // podman or docker on PATH, same namespace as lemond
    Toolbox,    // Fedora Toolbox / Distrobox: the tool runs on the host via flatpak-spawn
    Container,  // lemond is itself a container talking to a bind-mounted socket
    Snap,       // strictly confined snap driving the Docker snap's daemon
};

const char* host_transport_name(HostTransport transport);

// URL of the container prerequisites page, optionally anchored at the section
// that fixes a ReadinessResult's `remediation_id`.
std::string container_prerequisites_url(const std::string& remediation_id = "");

// One per-arch object of a container backend's backend_versions.json entry: the
// pinned image and the hardware access its container gets. `tag` is for
// readers only; `digest` is what gets pulled, so an upstream retag cannot
// change what a release of Lemonade runs.
struct ContainerImage {
    std::string repository;
    std::string tag;
    std::string digest;
    std::string channel;  // "stable" | "experimental"
    std::vector<std::string> devices;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> cap_add;
    bool ipc_host = false;
    bool memlock_unlimited = false;

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

struct ContainerMount {
    std::string host_path;  // a host directory or file, or a volume name when `volume`
    std::string container_path;
    bool read_only = true;
    // Set when host_path names a volume rather than a host path. The tool then
    // mounts `volume_subpath` inside that volume; "" is the whole volume.
    // Docker calls this volume-subpath, podman subpath.
    bool volume = false;
    std::string volume_subpath;
};

// Everything one `run` of a container needs, as plain data.
struct ContainerRunSpec {
    std::string name;
    ContainerImage image;
    std::vector<std::pair<std::string, std::string>> labels;
    std::vector<std::string> groups;  // numeric gids, or podman's keep-groups
    std::vector<ContainerMount> mounts;
    std::vector<std::pair<std::string, std::string>> env;  // on top of the image's
    std::string network;  // a network name, or "container:<id>"
    bool publish_port = true;
    int host_port = 0;
    int container_port = 0;
    std::vector<std::string> command;  // argv appended after the image
};

// Why a container backend cannot run right now. Each value maps to a section
// id on the container prerequisites doc page, so /system-info can hand the user
// a link that lands on the fix for their specific problem.
enum class ContainerReadiness {
    Ready,
    NoContainerTool,
    ContainerToolUnreachable,
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

// Invocation of podman or docker, injectable so the whole layer can be tested
// against a fake binary.
using CommandRunner = std::function<CommandResult(const std::string& executable,
                                                  const std::vector<std::string>& args,
                                                  int timeout_seconds)>;

// The one podman/docker wrapper in lemond, and the only code that invokes
// either tool.
class ContainerManager {
public:
    // What ContainerManager found when it probed for podman and docker.
    struct Info {
        ContainerTool tool = ContainerTool::None;
        std::string executable;  // absolute path, as resolved on PATH
        std::string version;

        std::string name() const {
            switch (tool) {
                case ContainerTool::Podman: return "podman";
                case ContainerTool::Docker: return "docker";
                case ContainerTool::None:   return "";
            }
            return "";
        }
    };

    explicit ContainerManager(CommandRunner runner = nullptr,
                              std::optional<HostTransport> transport = std::nullopt);

    static ContainerManager& global();

    // --- podman/docker discovery -------------------------------------------
    HostTransport transport() const { return transport_; }
    static HostTransport detect_transport();
    // Podman first, then docker, each verified by a call that reaches its
    // daemon or socket. Cached.
    const std::optional<Info>& info();
    // The executable and leading argv for this transport. In a toolbox that is
    // `flatpak-spawn --host <tool>`; a podman reached over a socket gets
    // `--remote`.
    std::pair<std::string, std::vector<std::string>> invocation(const Info& info) const;
    // Container transport: the id of the container lemond runs in, from the
    // tool's own view, or "" when it cannot be determined.
    std::string self_container_id();
    // Container transport: lemond's own mounts as (host source -> path inside
    // lemond's container).
    const std::vector<ContainerMount>& self_mounts();

    // Can this host give a container access to `devices`? Returns the first
    // blocking problem.
    ReadinessResult check_readiness(const std::vector<std::string>& devices);

    // --- run command ---------------------------------------------------------
    // The mount that exposes `visible_path`, a path as this process sees it, at
    // `container_path`, with its source translated for the host.
    ContainerMount host_mount(const std::string& visible_path,
                              const std::string& container_path);
    // The --group-add values that give a container access to `devices`: the
    // user's own groups under podman, the host's video and render gids under
    // docker.
    std::vector<std::string> group_adds(const std::vector<std::string>& devices);
    // The complete command line that runs `spec`, starting with the executable.
    std::vector<std::string> run_command(const ContainerRunSpec& spec);

    // --- images --------------------------------------------------------------
    // Every locally present digest for `repository`. Several backends of one
    // recipe are usually different tags of the same repository, so an image is
    // identified by its own digest, never by "the digest this repository has".
    std::vector<std::string> installed_digests(const std::string& repository);
    bool has_image_digest(const std::string& repository, const std::string& digest);
    // First locally present digest, or "": what is installed when it is not
    // the pinned digest.
    std::string installed_digest(const std::string& repository);
    void pull(const ContainerImage& image, DownloadProgressCallback progress = nullptr);
    void remove_image(const ContainerImage& image);

    // --- containers ----------------------------------------------------------
    // Stops the container, removes it and its private network. Call before
    // killing the client: SIGKILL is not forwarded into the container, which
    // would go on holding the GPU.
    void stop(const std::string& name);
    void stop_container(const std::string& name, int timeout_seconds = 10);
    void remove_container(const std::string& name);
    // The container's address on its network, or "" while it has none yet.
    std::string container_address(const std::string& name);
    // Idempotent; `--internal`, carrying the managed label.
    void ensure_isolated_network(const std::string& name);
    void remove_network(const std::string& name);
    std::vector<std::string> list_managed_networks();
    std::vector<std::string> list_managed_containers();
    // Removes every container and private network carrying the managed label,
    // so a killed lemond does not leave GPU-holding containers behind.
    int sweep_managed_containers();

    // --- pure helpers (no podman or docker needed) ----------------------------
    static std::vector<std::string> build_run_args(const ContainerRunSpec& spec,
                                                   ContainerTool tool);
    static std::vector<std::string> build_pull_args(const ContainerImage& image);
    static std::vector<std::string> build_stop_args(const std::string& name, int timeout_seconds);

    // Container name for humans reading `ps`. Ownership is the label, not the
    // name.
    static std::string container_name(const std::string& recipe, const std::string& backend,
                                      int port);
    // The bare label every managed container carries.
    static const char* managed_label();

    // Host gid for a group name as a decimal string, or "" when undefined.
    // `--group-add <name>` resolves against the image's group file, which may
    // not define the name or may map it to a different gid than the device
    // node checks.
    static std::string host_group_gid(const std::string& name);

    // Parse `<tool> --version` output into a bare version string.
    static std::string parse_version(const std::string& output);
    // Digests for `repository` in `<tool> images --digests` output, whose lines
    // are "<repository>@<digest>".
    static std::vector<std::string> parse_repo_digests(const std::string& output,
                                                       const std::string& repository);

    // --- GPU selection ---------------------------------------------------------
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
    // source is the volume name and podman or docker resolves it.
    static std::vector<ContainerMount> parse_self_mounts(const std::string& inspect_json);

private:
    CommandResult run(const std::vector<std::string>& args, int timeout_seconds);
    CommandResult invoke(const Info& info, const std::vector<std::string>& args,
                         int timeout_seconds);
    std::optional<Info> probe(ContainerTool tool, const std::string& binary);

    CommandRunner runner_;
    HostTransport transport_;
    std::optional<Info> info_;
    bool info_probed_ = false;
    bool self_probed_ = false;
    std::string self_id_;
    std::vector<ContainerMount> self_mounts_;
};

}  // namespace utils
}  // namespace lemon
