#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
struct DownloadProgress;
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;
}

namespace lemon {
namespace utils {

class ContainerAddressPending : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ContainerTool { Podman, Docker };

struct ContainerMount {
    std::string source;
    std::string destination;
};
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
    std::string image;
    std::vector<std::string> command;
};
struct SetupFailure {
    std::string id;
    std::string message;
    std::vector<std::string> steps;
    std::string apply;
};
struct SetupReport {
    std::vector<SetupFailure> failures;

    bool empty() const { return failures.empty(); }
    std::vector<std::string> ids() const;
    std::string message() const;
    std::string action() const;
    std::string text() const;
};

struct CommandResult {
    int exit_code = -1;
    std::string output;
};
enum class ContainerFileKind { File, Directory, Other };

struct ContainerPin {
    std::string tag;
    std::string digest;
};

std::optional<ContainerPin> parse_container_pin(const std::string& pin);
std::string short_container_pin(const std::string& pin);
std::string container_image(const std::string& repository, const std::string& pin);

struct ContainerHost {
    std::function<bool(const std::string& name)> on_path;
    std::function<bool()> gpu_devices_accessible;
    std::function<std::string(const std::string& group)> group_id;
    std::function<std::string()> account;
    std::function<std::string(const std::string& name)> env;
    std::function<bool()> is_root;
    std::function<std::string(const std::string& path)> read_file;
    std::function<CommandResult(const std::vector<std::string>& argv, int timeout_seconds,
                                bool capture_stderr)>
        run;

    std::function<int(const std::vector<std::string>& argv, int timeout_seconds,
                      const std::function<bool(const std::string&)>& on_line)>
        stream;
    std::function<std::string(const std::string& path)> canonical;
    std::function<ContainerFileKind(const std::string& path)> file_kind;
    std::function<std::vector<std::string>(const std::string& path)> files;
    std::function<void(std::chrono::milliseconds)> sleep;

    static ContainerHost real();
};
class ContainerManager {
public:
    explicit ContainerManager(ContainerHost host = ContainerHost::real());

    std::optional<ContainerTool> tool() const;
    bool rootless_podman() const;
    std::vector<SetupFailure> check_host() const;
    std::optional<SetupFailure> check_podman() const;
    std::optional<SetupFailure> check_selinux() const;

    std::vector<std::string> run_command(const ContainerRunSpec& spec) const;

    bool has_image(const std::string& image) const;
    void pull(const std::string& image, const DownloadProgressCallback& progress) const;
    void remove_image(const std::string& image) const;
    void create_network(const std::string& name) const;
    void remove_container(const std::string& name) const;
    void stop(const std::string& name) const;
    std::string address(const std::string& name, const std::string& network,
                        int timeout_seconds = 5) const;
    static std::string container_name(const std::string& recipe, const std::string& backend,
                                      const std::string& model);
    static std::string podman_install_command(const std::string& os_release);
    static bool allowed_image(const std::string& image);
    std::vector<ContainerMount> model_mounts(const std::string& host_path,
                                             const std::string& destination) const;

private:
    CommandResult invoke(const std::vector<std::string>& args, int timeout_seconds,
                         bool capture_stderr = true) const;
    void remove_network(const std::string& name) const;
    static void require_image(const std::string& image);
    std::optional<bool> podman_info_rootless() const;
    bool expect_rootless() const;

    ContainerHost host_;
    mutable std::mutex podman_mode_mutex_;
    mutable std::optional<bool> podman_rootless_;
    mutable std::string podman_network_backend_;
};

}
}
