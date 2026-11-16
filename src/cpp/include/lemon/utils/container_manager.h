#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
struct DownloadProgress;
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;
}  // namespace lemon

namespace lemon {
namespace utils {

enum class ContainerTool { Podman, Docker };

struct ContainerMount {
    std::string source;       // path on the host
    std::string destination;  // path inside the container, under /mnt/models
};

// Everything one `run` needs, as plain data; the per-tool options are added
// when the command is built.
struct ContainerRunSpec {
    std::string name;
    std::string recipe;
    std::string backend;
    std::string model;
    int port = 0;
    std::string network;
    // A PID namespace to join, such as container:<id>, or "" for the
    // container's own.
    std::string pid;
    std::vector<std::string> devices;
    std::vector<std::string> cap_add;
    std::vector<ContainerMount> mounts;
    std::vector<std::pair<std::string, std::string>> env;
    std::string image;                 // <image>@<digest>
    std::vector<std::string> command;  // program and its arguments
};

// A failed setup check. `id` names the check for clients, `message` says what
// is missing and `steps` are the commands that fix it. `apply` is the step that
// makes a fix take effect, such as logging out and back in, which a report
// lists once, after every other step.
struct SetupFailure {
    std::string id;
    std::string message;
    std::vector<std::string> steps;
    std::string apply;
};

// Every setup step a container backend still needs, in the order the user
// takes them.
struct SetupReport {
    std::vector<SetupFailure> failures;

    bool empty() const { return failures.empty(); }
    std::vector<std::string> ids() const;
    // One failure's message per line.
    std::string message() const;
    // One step per line, numbered when there is more than one.
    std::string action() const;
    // The messages and steps as an indented list, for error text.
    std::string text() const;
};

struct CommandResult {
    int exit_code = -1;
    std::string output;  // stdout and stderr interleaved
};

// Each host read, defaulting to the real host so tests can fake every branch.
struct ContainerHost {
    std::function<bool(const std::string& name)> on_path;
    std::function<bool(const std::string& path)> exists;
    // True when this account can read and write /dev/kfd and every /dev/dri
    // render node present on the host.
    std::function<bool()> gpu_devices_accessible;
    // The numeric group that owns /dev/kfd or the first /dev/dri render node,
    // or "" when neither is present.
    std::function<std::string()> gpu_device_group;
    // The group's numeric ID, or "" when the host defines no such group.
    std::function<std::string(const std::string& group)> group_id;
    // The name of the account this process runs as, or "" when it is unknown.
    std::function<std::string()> account;
    // The environment variable's value, or "" when it is unset.
    std::function<std::string(const std::string& name)> env;
    std::function<bool()> is_root;
    std::function<std::string(const std::string& path)> read_file;
    // Runs a command line and waits for it. Without `capture_stderr`, stderr
    // stays out of `output`, since Podman prints warnings there even when the
    // command succeeds.
    std::function<CommandResult(const std::vector<std::string>& argv, int timeout_seconds,
                                bool capture_stderr)>
        run;

    static ContainerHost real();
};

// The one object in lemond that runs Podman or Docker, the container
// counterpart of ProcessManager.
class ContainerManager {
public:
    explicit ContainerManager(ContainerHost host = ContainerHost::real());

    static ContainerManager& global();

    // Podman when it is on PATH, Docker otherwise, nullopt when neither is. In
    // a container, the tool whose host socket is mounted.
    std::optional<ContainerTool> tool() const;
    // Only rootless Podman publishes a port from an --internal network; under
    // rootful Podman or Docker, lemond connects at the container's address.
    bool rootless_podman() const;

    // Every failed check of the tool on PATH and the account's access to it.
    // Rerun on each call, so a fix takes effect without a restart.
    std::vector<SetupFailure> check_host() const;
    // lemond.service's subordinate ID ranges, then `podman info`.
    std::optional<SetupFailure> check_podman() const;
    // The SELinux check for a backend whose devices include /dev/kfd, or
    // nullopt when it passes.
    std::optional<SetupFailure> check_selinux() const;

    std::vector<std::string> run_command(const ContainerRunSpec& spec) const;

    // True when lemond itself runs in a container, from the marker file Docker
    // or Podman puts in every container.
    bool in_container() const;
    // The ID of the container lemond runs in, as the tool behind CONTAINER_HOST
    // or DOCKER_HOST knows it, or "" when lemond is not in a container or that
    // tool cannot find it. Backends then run beside lemond's container on the
    // same host tool, share its network, and mount model files by their host
    // paths.
    std::string own_container() const;
    // The path on the host for `path` inside lemond's container, through the
    // bind mount or volume that holds it. Throws when no mount holds it.
    std::string host_path(const std::string& path) const;
    // lemonade-backends when lemond's container is on it and it is internal,
    // otherwise "".
    std::string backend_network() const;

    bool has_image(const std::string& image) const;
    void pull(const std::string& image, const DownloadProgressCallback& progress) const;
    void remove_image(const std::string& image) const;

    // Replaces any network of that name with an --internal one carrying the
    // ai.lemonade label.
    void create_network(const std::string& name) const;
    void remove_container(const std::string& name) const;
    // Stops the container with SIGTERM and SIGKILL after 10 seconds, then
    // removes it and its network.
    void stop(const std::string& name) const;
    // The container's address on its network, or "" while it has none.
    std::string address(const std::string& name) const;

    // lemonade-<instance>-<recipe>-<backend>-<model>, reduced to the
    // characters a container name accepts. `instance` is the port the owning
    // lemond listens on, or the short ID of the container it runs in, so two
    // lemond instances sharing a container daemon never claim each other's
    // containers. Every lemond in a container listens on the same port.
    static std::string container_name(const std::string& instance, const std::string& recipe,
                                      const std::string& backend, const std::string& model);
    // The command that installs `package` on the distribution `os_release`
    // describes, matched on ID, then each word of ID_LIKE. `name` is how the
    // generic step for an unknown distribution refers to it.
    static std::string install_command(const std::string& os_release, const std::string& package,
                                       const std::string& name);
    static bool allowed_image(const std::string& image);
    // Read-only mounts that put `host_path` at `destination`, each source
    // resolved through symlinks. A directory becomes one mount per file inside
    // it, because a Hugging Face cache's links point into a blobs/ folder the
    // container would not see. Throws when `host_path` does not exist.
    static std::vector<ContainerMount> model_mounts(const std::string& host_path,
                                                    const std::string& destination);

private:
    CommandResult invoke(const std::vector<std::string>& args, int timeout_seconds,
                         bool capture_stderr = true) const;
    void remove_network(const std::string& name) const;
    // `podman info`'s answer, nullopt while it fails. Cached once it answers,
    // since setup checks run on every /system-info request.
    std::optional<bool> podman_info_rootless() const;
    // `podman info`'s answer once it has given one. Before that, rootless
    // unless lemond runs as root or CONTAINER_HOST points Podman somewhere
    // other than the account's own podman.socket.
    bool expect_rootless() const;
    bool own_podman_socket() const;
    static bool needs_crun(bool rootless, const std::string& runtime);
    // The host's render group ID, for a tool that adds it by number. In a
    // container, LEMONADE_RENDER_GID, then the GPU device nodes' owner, since
    // the image's /etc/group says nothing about the host.
    std::string render_group_id() const;
    // IDs the container lemond runs in may go by, most specific first.
    std::vector<std::string> own_container_candidates() const;

    struct OwnMount {
        std::string destination;
        std::string source;
    };

    ContainerHost host_;
    mutable std::mutex podman_mode_mutex_;
    mutable std::optional<bool> podman_rootless_;
    // Podman's network backend, "netavark" or "cni", from the same answer.
    mutable std::string podman_network_backend_;
    // Podman's OCI runtime, such as "crun" or "runc", from the same answer.
    mutable std::string podman_oci_runtime_;
    // lemond's own container and its mounts, once the tool has found it.
    mutable std::mutex own_container_mutex_;
    mutable std::string own_container_;
    mutable std::vector<OwnMount> own_mounts_;
    mutable std::string backend_network_;
};

}  // namespace utils
}  // namespace lemon
