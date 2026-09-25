#pragma once

#include <chrono>
#include <functional>
#include <mutex>
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
// process namespace, so its calls and mount sources are translated.
enum class HostTransport {
    Native,     // podman or docker on PATH, same namespace as lemond
    Toolbox,    // Fedora Toolbox / Distrobox: the tool runs on the host via flatpak-spawn
    Container,  // the Lemonade Docker image, talking to a socket mounted into it
    Snap,       // the lemonade-server snap, talking through its podman or docker plug
};

const char* host_transport_name(HostTransport transport);

// One per-arch object of a container backend's backend_versions.json entry: the
// pinned image and the hardware access its container gets. `tag` is for
// readers only; `digest` is what gets pulled, so an upstream retag cannot
// change what a release of Lemonade runs.
struct ContainerImage {
    std::string repository;
    std::string tag;
    std::string digest;
    std::vector<std::string> devices;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> cap_add;
    bool ipc_host = false;
    bool memlock_unlimited = false;

    bool valid() const { return !repository.empty() && !digest.empty(); }
    std::string pinned_ref() const { return repository + "@" + digest; }
    std::string tagged_ref() const { return repository + ":" + tag; }
};

struct ContainerMount {
    std::string host_path;  // a host directory or file, or a volume name when `volume`
    std::string container_path;
    bool read_only = true;
    // Set when host_path names a volume rather than a host path. The tool then
    // mounts `volume_subpath` inside that volume; "" is the whole volume.
    bool volume = false;
    std::string volume_subpath;
};

// Everything one `run` of a container needs, as plain data.
struct ContainerRunSpec {
    std::string name;
    ContainerImage image;
    std::vector<std::pair<std::string, std::string>> labels;
    std::vector<ContainerMount> mounts;
    // On top of the image's env; a key here replaces the image's value.
    std::vector<std::pair<std::string, std::string>> env;
    std::string network;  // a network name, or "container:<id>"
    int port = 0;
    std::vector<std::string> command;  // argv appended after the image
};

// The first failing setup check for a container backend. `message` is the
// check's "fails when" text and `action` the commands that fix it, one per line.
struct ReadinessResult {
    bool ok = true;
    std::string message;
    std::string action;
    // False when the tool itself cannot be reached, so nothing, not even a
    // pull, can run.
    bool tool_usable = true;
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
// either tool. Safe to call from concurrent request handlers.
class ContainerManager {
public:
    struct Info {
        ContainerTool tool = ContainerTool::None;
        std::string executable;

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
    // The tool this host uses: podman when it is installed, docker otherwise.
    // What counts as installed depends on the host (see is_installed).
    std::optional<Info> info();
    // The executable and leading argv for this transport. In a toolbox that is
    // `flatpak-spawn --host <tool>`; podman gets `--remote` when CONTAINER_HOST
    // names its socket.
    std::pair<std::string, std::vector<std::string>> invocation(const Info& info) const;
    // Container transport: the id of the container lemond runs in, or "" when
    // it cannot inspect itself.
    std::string self_container_id();

    // The setup checks for this host, in order, stopping at the first failure.
    // Rerun on every call, so a fix takes effect without a restart.
    ReadinessResult check_readiness(const std::vector<std::string>& devices);

    // --- run command ---------------------------------------------------------
    // The mount that exposes `visible_path`, a path as this process sees it, at
    // `container_path`, with its source translated for the host.
    ContainerMount host_mount(const std::string& visible_path,
                              const std::string& container_path);
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
    void remove_container(const std::string& name);
    // The container's address on its network, or "" while it has none yet.
    std::string container_address(const std::string& name);
    // Idempotent; `--internal`, carrying the managed label.
    void ensure_isolated_network(const std::string& name);
    void remove_network(const std::string& name);
    // Removes every container, stopped ones included, and every private
    // network carrying the managed label.
    int sweep_managed_containers();

    // --- pure helpers (no podman or docker needed) ----------------------------
    // Every Podman and Docker difference in the run command lives here.
    static std::vector<std::string> build_run_args(const ContainerRunSpec& spec,
                                                   ContainerTool tool);
    // True when lemond reaches the server at 127.0.0.1:<port>, false when at
    // the container's address on its private network.
    static bool connects_on_loopback(const ContainerRunSpec& spec, ContainerTool tool);
    static std::vector<std::string> build_pull_args(const ContainerImage& image);
    static std::vector<std::string> build_stop_args(const std::string& name, int timeout_seconds);

    // Container name for humans reading `ps`. Ownership is the label, not the
    // name. The router loads each model at most once, so the model makes it
    // unique.
    static std::string container_name(const std::string& recipe, const std::string& backend,
                                      const std::string& model);
    static const char* managed_label();

    // Host gid for a group name as a decimal string, or "" when undefined.
    // `--group-add <name>` resolves against the image's group file, which may
    // not define the name or may map it to a different gid than the host's.
    static std::string host_group_gid(const std::string& name);

    // The command that installs podman on the distribution an os-release file
    // describes, matched on ID, then each word of ID_LIKE.
    static std::string podman_install_command(const std::string& os_release_text);

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
    bool is_installed(ContainerTool tool);
    bool reachable(const Info& info);
    bool started_by_service() const;
    std::string install_command() const;
    void probe_self();
    std::optional<Info> select_tool();

    CommandRunner runner_;
    HostTransport transport_;
    std::recursive_mutex mutex_;
    std::optional<Info> info_;
    bool self_probed_ = false;
    std::string self_id_;
    std::vector<ContainerMount> self_mounts_;
    std::vector<std::string> readiness_devices_;
    std::optional<ReadinessResult> readiness_;
    std::chrono::steady_clock::time_point readiness_time_;
};

}  // namespace utils
}  // namespace lemon
