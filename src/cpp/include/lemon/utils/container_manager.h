#pragma once

#include <functional>
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

// A host file the container reads, mounted read-only.
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

// A failed setup check: `message` says what is missing and `action` holds the
// commands that fix it, one step per line.
struct SetupFailure {
    std::string message;
    std::string action;

    std::string text() const { return message + ". To fix it:\n" + action; }
};

struct CommandResult {
    int exit_code = -1;
    std::string output;  // stdout and stderr interleaved
};

// Each host read, defaulting to the real host so tests can fake every branch.
struct ContainerHost {
    // True when `name` is an executable on PATH.
    std::function<bool(const std::string& name)> on_path;
    // True when this process belongs to `group`.
    std::function<bool(const std::string& group)> in_group;
    // The group's numeric ID, or "" when the host defines no such group.
    std::function<std::string(const std::string& group)> group_id;
    // True when /var/run/docker.sock accepts a connection from this process.
    std::function<bool()> docker_socket_accepts;
    // The contents of a file, or "" when it cannot be read.
    std::function<std::string(const std::string& path)> read_file;
    // Runs a command line and waits for it.
    std::function<CommandResult(const std::vector<std::string>& argv, int timeout_seconds)> run;

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

    // The first setup check that fails for a container backend, or nullopt
    // when every check passes. Rerun on each call, so a fix takes effect
    // without a restart.
    std::optional<SetupFailure> check_setup() const;
    // The SELinux check for a backend whose devices include /dev/kfd, or
    // nullopt when it passes.
    std::optional<SetupFailure> check_selinux() const;

    // The complete `run` command line for `spec`, starting with the tool.
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
    // True for the images container backends may pull.
    static bool allowed_image(const std::string& image);
    // "gfx1151" for KFD's gfx_target_version 110501.
    static std::string gfx_name(int gfx_target_version);
    // HIP_VISIBLE_DEVICES for the first GPU whose name is `arch`, counting the
    // GPU nodes (gfx_target_version != 0) in KFD node order; "" when none is.
    static std::string gpu_index(const std::vector<int>& gfx_target_versions,
                                 const std::string& arch);
    // The same, read from /sys/devices/virtual/kfd/kfd/topology/nodes.
    static std::string kfd_gpu_index(const std::string& arch);
    // Read-only mounts that put `host_path` at `destination`, each source
    // resolved through symlinks. A directory becomes one mount per file inside
    // it, because a Hugging Face cache's links point into a blobs/ folder the
    // container would not see. Throws when `host_path` does not exist.
    static std::vector<ContainerMount> model_mounts(const std::string& host_path,
                                                    const std::string& destination);

private:
    CommandResult invoke(const std::vector<std::string>& args, int timeout_seconds) const;
    void remove_network(const std::string& name) const;

    ContainerHost host_;
};

}  // namespace utils
}  // namespace lemon
