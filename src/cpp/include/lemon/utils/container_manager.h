#pragma once

#include <functional>
#include <map>
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
    std::function<bool(const std::string& group)> in_group;
    // The group's numeric ID, or "" when the host defines no such group.
    std::function<std::string(const std::string& group)> group_id;
    // True when /var/run/docker.sock accepts a connection from this process.
    std::function<bool()> docker_socket_accepts;
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

    // Podman when it is on PATH, Docker otherwise, nullopt when neither is.
    std::optional<ContainerTool> tool() const;
    // Only rootless Podman publishes a port from an --internal network; under
    // rootful Podman or Docker, lemond connects at the container's address.
    bool rootless_podman() const;

    // Every failed check that runs before the user enables container
    // backends: the tool on PATH, then the account's access to it. These read
    // only PATH, groups and sockets, so no container command runs before the
    // user opts in. Rerun on each call, so a fix takes effect without a
    // restart.
    std::vector<SetupFailure> check_host() const;
    // `podman info`, which runs only once container backends are enabled.
    std::optional<SetupFailure> check_podman() const;
    // The SELinux check for a backend whose devices include /dev/kfd, or
    // nullopt when it passes.
    std::optional<SetupFailure> check_selinux() const;

    std::vector<std::string> run_command(const ContainerRunSpec& spec) const;

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
    // Removes every container, stopped ones included, and every network that
    // carries the ai.lemonade label.
    void sweep() const;

    // lemonade-<recipe>-<backend>-<model>, reduced to the characters a
    // container name accepts.
    static std::string container_name(const std::string& recipe, const std::string& backend,
                                      const std::string& model);
    // The command that installs Podman on the distribution `os_release`
    // describes, matched on ID, then each word of ID_LIKE.
    static std::string podman_install_command(const std::string& os_release);
    static bool allowed_image(const std::string& image);
    // "gfx1151" for KFD's gfx_target_version 110501.
    static std::string gfx_name(int gfx_target_version);
    // HIP_VISIBLE_DEVICES for the first GPU whose name is `arch`, counting the
    // GPU nodes (gfx_target_version != 0) in KFD node order; "" when none is.
    static std::string gpu_index(const std::vector<int>& gfx_target_versions,
                                 const std::string& arch);
    // The same, read from /sys/devices/virtual/kfd/kfd/topology/nodes.
    static std::string kfd_gpu_index(const std::string& arch);
    // Each KFD topology node's properties file, in node order.
    static std::vector<std::string> kfd_node_properties();
    static std::map<std::string, unsigned long long> parse_kfd_properties(
        const std::string& properties);
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
    // unless CONTAINER_HOST points Podman elsewhere or lemond runs as root.
    bool expect_rootless() const;

    ContainerHost host_;
    mutable std::mutex podman_mode_mutex_;
    mutable std::optional<bool> podman_rootless_;
};

}  // namespace utils
}  // namespace lemon
