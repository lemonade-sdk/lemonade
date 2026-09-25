#include "lemon/utils/container_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <lemon/utils/aixlog.hpp>
#include <nlohmann/json.hpp>
#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#ifndef _WIN32
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

namespace {

constexpr const char* kManagedPrefix = "lemonade-";
constexpr const char* kManagedLabel = "ai.lemonade";
constexpr const char* kKfdTopologyNodes = "/sys/devices/virtual/kfd/kfd/topology/nodes";
constexpr const char* kServiceAccount = "lemonade";
constexpr const char* kPodmanSocket = "/run/podman/podman.sock";
constexpr const char* kDockerSocket = "/var/run/docker.sock";
constexpr const char* kDockerImageGuide =
    "https://lemonade-server.ai/docs/guide/install/docker/#container-backends";
constexpr int kShortCommandTimeout = 30;
constexpr int kPullTimeout = 3600;
constexpr auto kReadinessReuse = std::chrono::seconds(2);

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (!trimmed.empty()) lines.push_back(trimmed);
    }
    return lines;
}

// A container name accepts [A-Za-z0-9][A-Za-z0-9_.-]*.
std::string sanitize_name_token(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (char c : token) {
        const unsigned char uc = static_cast<unsigned char>(c);
        out.push_back((std::isalnum(uc) || c == '-' || c == '_' || c == '.') ? c : '-');
    }
    return out;
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// True when this process belongs to every one of the video and render groups
// the host defines. Rootless podman carries only real group membership into
// its user namespace, so an ACL that logind grants the seat owner does not
// count.
bool in_video_and_render() {
#ifdef _WIN32
    return true;
#else
    const int count = ::getgroups(0, nullptr);
    std::vector<gid_t> groups(static_cast<size_t>(count > 0 ? count : 0));
    if (count > 0 && ::getgroups(count, groups.data()) < 0) groups.clear();
    for (const char* name : {"video", "render"}) {
        const struct group* entry = ::getgrnam(name);
        if (!entry) continue;
        const gid_t gid = entry->gr_gid;
        if (::getgid() == gid || ::getegid() == gid) continue;
        if (std::find(groups.begin(), groups.end(), gid) == groups.end()) return false;
    }
    return true;
#endif
}

ReadinessResult failure(std::string message, std::string action, bool tool_usable) {
    ReadinessResult result;
    result.ok = false;
    result.message = std::move(message);
    result.action = std::move(action);
    result.tool_usable = tool_usable;
    return result;
}

CommandResult default_runner(const std::string& executable,
                             const std::vector<std::string>& args,
                             int timeout_seconds) {
    CommandResult result;
    std::string output;
    result.exit_code = ProcessManager::run_process_with_output(
        executable, args,
        [&output](const std::string& line) {
            output += line;
            output += "\n";
            return true;
        },
        "", timeout_seconds, /*capture_stderr=*/true);
    result.output = output;
    return result;
}

}  // namespace

const char* host_transport_name(HostTransport transport) {
    switch (transport) {
        case HostTransport::Native: return "native";
        case HostTransport::Toolbox: return "toolbox";
        case HostTransport::Container: return "container";
        case HostTransport::Snap: return "snap";
    }
    return "native";
}

ContainerManager::ContainerManager(CommandRunner runner, std::optional<HostTransport> transport)
    : runner_(runner ? std::move(runner) : CommandRunner(default_runner)),
      transport_(transport ? *transport : detect_transport()) {}

ContainerManager& ContainerManager::global() {
    static ContainerManager instance;
    return instance;
}

HostTransport ContainerManager::detect_transport() {
#ifdef _WIN32
    return HostTransport::Native;
#else
    if (!env_or_empty("SNAP_NAME").empty()) return HostTransport::Snap;
    if (fs::exists("/run/.toolboxenv")) return HostTransport::Toolbox;
    if (fs::exists("/.dockerenv") || fs::exists("/run/.containerenv")) {
        return HostTransport::Container;
    }
    return HostTransport::Native;
#endif
}

std::pair<std::string, std::vector<std::string>> ContainerManager::invocation(
    const Info& info) const {
    std::vector<std::string> prefix;
    std::string executable = info.executable;
    if (transport_ == HostTransport::Toolbox) {
        executable = find_executable_in_path("flatpak-spawn");
        if (executable.empty()) executable = "flatpak-spawn";
        prefix = {"--host", info.name()};
    }
    // The Docker image and the snap bundle only a podman client, which has to
    // reach the host's podman over its socket.
    const bool remote = !env_or_empty("CONTAINER_HOST").empty() ||
                        transport_ == HostTransport::Container ||
                        transport_ == HostTransport::Snap;
    if (info.tool == ContainerTool::Podman && remote) prefix.push_back("--remote");
    return {executable, prefix};
}

const char* ContainerManager::managed_label() { return kManagedLabel; }

std::string ContainerManager::container_name(const std::string& recipe,
                                             const std::string& backend,
                                             const std::string& model) {
    return std::string(kManagedPrefix) + sanitize_name_token(recipe) + "-" +
           sanitize_name_token(backend) + "-" + sanitize_name_token(model);
}

std::string ContainerManager::host_group_gid(const std::string& name) {
#ifdef _WIN32
    (void)name;
    return "";
#else
    if (const struct group* entry = ::getgrnam(name.c_str())) {
        return std::to_string(static_cast<unsigned long>(entry->gr_gid));
    }
    return "";
#endif
}

std::string ContainerManager::podman_install_command(const std::string& os_release_text) {
    std::string id;
    std::string id_like;
    for (const auto& line : split_lines(os_release_text)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "ID") id = value;
        if (key == "ID_LIKE") id_like = value;
    }

    std::vector<std::string> candidates;
    if (!id.empty()) candidates.push_back(id);
    std::istringstream words(id_like);
    for (std::string word; words >> word;) candidates.push_back(word);

    for (const auto& candidate : candidates) {
        if (candidate == "debian") return "sudo apt install podman";
        if (candidate == "fedora") return "sudo dnf install podman";
        if (candidate == "arch") return "sudo pacman -S podman";
    }
    return "Install Podman with the host's package manager";
}

std::vector<std::string> ContainerManager::parse_repo_digests(const std::string& output,
                                                              const std::string& repository) {
    // Docker Hub images are commonly stored without the "docker.io/" prefix, so
    // the bare form matches too.
    std::string bare = repository;
    const std::string kDockerIo = "docker.io/";
    if (bare.rfind(kDockerIo, 0) == 0) bare = bare.substr(kDockerIo.size());

    std::vector<std::string> digests;
    for (const auto& line : split_lines(output)) {
        const auto at = line.find('@');
        if (at == std::string::npos) continue;
        const std::string repo = line.substr(0, at);
        const std::string digest = line.substr(at + 1);
        // An image pulled by tag alone renders "<none>" here, and a tool with
        // nothing to show renders "<no value>". Neither is a digest.
        if (digest.rfind("sha256:", 0) != 0) continue;
        if (repo == repository || repo == bare) digests.push_back(digest);
    }
    return digests;
}

std::string ContainerManager::gfx_name_from_target_version(int target_version) {
    if (target_version <= 0) return "";
    const int major = target_version / 10000;
    const int minor = (target_version / 100) % 100;
    const int step = target_version % 100;
    static const char* hex = "0123456789abcdef";
    std::string name = "gfx" + std::to_string(major);
    name.push_back(minor < 16 ? hex[minor] : '?');
    name.push_back(step < 16 ? hex[step] : '?');
    return name;
}

std::string ContainerManager::pick_gpu_index(const std::vector<int>& target_versions,
                                             const std::string& arch) {
    if (arch.empty()) return "";
    int gpu_index = 0;
    for (const int version : target_versions) {
        if (version == 0) continue;  // a CPU node
        if (gfx_name_from_target_version(version) == arch) return std::to_string(gpu_index);
        ++gpu_index;
    }
    return "";
}

std::string ContainerManager::kfd_gpu_index_for_arch(const std::string& arch) {
    std::error_code ec;
    if (!fs::is_directory(kKfdTopologyNodes, ec)) return "";
    std::vector<fs::path> nodes;
    for (const auto& entry : fs::directory_iterator(kKfdTopologyNodes, ec)) {
        nodes.push_back(entry.path());
    }
    std::sort(nodes.begin(), nodes.end(), [](const fs::path& a, const fs::path& b) {
        return std::stol(a.filename().string()) < std::stol(b.filename().string());
    });
    std::vector<int> versions;
    for (const auto& node : nodes) {
        std::ifstream props(node / "properties");
        std::string key;
        long value = 0;
        int version = 0;
        while (props >> key >> value) {
            if (key == "gfx_target_version") version = static_cast<int>(value);
        }
        versions.push_back(version);
    }
    return pick_gpu_index(versions, arch);
}

std::vector<ContainerMount> ContainerManager::parse_self_mounts(const std::string& inspect_json) {
    std::vector<ContainerMount> mounts;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(inspect_json);
    } catch (...) {
        return mounts;
    }
    // `inspect` prints a one-element array for a single container;
    // `--format {{json .Mounts}}` prints the array of mounts directly.
    if (parsed.is_array() && !parsed.empty() && parsed[0].is_object() &&
        parsed[0].contains("Mounts")) {
        parsed = parsed[0]["Mounts"];
    }
    if (!parsed.is_array()) return mounts;
    for (const auto& entry : parsed) {
        if (!entry.is_object()) continue;
        const std::string source = entry.value("Source", "");
        const std::string destination = entry.value("Destination", "");
        const std::string type = entry.value("Type", "");
        // A named volume's Source is the tool's private storage directory,
        // unreachable for rootless podman. Its Name is what a mount resolves.
        ContainerMount mount;
        mount.container_path = destination;
        mount.read_only = !entry.value("RW", true);
        if (type == "volume" && entry.contains("Name")) {
            mount.volume = true;
            mount.host_path = entry.value("Name", "");
        } else {
            mount.host_path = source;
        }
        if (mount.host_path.empty() || destination.empty()) continue;
        mounts.push_back(mount);
    }
    return mounts;
}

std::vector<std::string> ContainerManager::build_pull_args(const ContainerImage& image) {
    return {"pull", image.pinned_ref()};
}

std::vector<std::string> ContainerManager::build_stop_args(const std::string& name,
                                                           int timeout_seconds) {
    return {"stop", "--time", std::to_string(timeout_seconds), name};
}

bool ContainerManager::connects_on_loopback(const ContainerRunSpec& spec, ContainerTool tool) {
    const bool shared_namespace = spec.network.rfind("container:", 0) == 0;
    return shared_namespace || tool == ContainerTool::Podman;
}

std::vector<std::string> ContainerManager::build_run_args(const ContainerRunSpec& spec,
                                                          ContainerTool tool) {
    std::vector<std::string> args = {"run", "--rm", "--init", "--name", spec.name,
                                     "--label", kManagedLabel};
    for (const auto& [key, value] : spec.labels) {
        args.push_back("--label");
        args.push_back(key + "=" + value);
    }

    args.push_back("--cap-drop=all");
    args.push_back("--security-opt=no-new-privileges");
    // Relabeling a bind mount that other programs share (the model cache) would
    // retag every file in it, so SELinux separation is off for the container.
    args.push_back("--security-opt=label=disable");
    args.push_back("--pull=never");
    args.push_back("--network=" + spec.network);

    for (const auto& device : spec.image.devices) {
        args.push_back("--device");
        args.push_back(device);
    }
    if (!spec.image.devices.empty()) {
        if (tool == ContainerTool::Podman) {
            args.push_back("--group-add");
            args.push_back("keep-groups");
        } else {
            // Docker resolves a group name in the image's /etc/group, so the
            // host's numbers are what match the device nodes.
            for (const char* group : {"video", "render"}) {
                const std::string gid = host_group_gid(group);
                if (gid.empty()) continue;
                args.push_back("--group-add");
                args.push_back(gid);
            }
        }
    }
    for (const auto& cap : spec.image.cap_add) {
        args.push_back("--cap-add");
        args.push_back(cap);
    }
    if (spec.image.ipc_host) {
        args.push_back("--ipc=host");
    }
    if (spec.image.memlock_unlimited) {
        args.push_back("--ulimit");
        args.push_back("memlock=-1:-1");
    }

    for (const auto& mount : spec.mounts) {
        args.push_back("--mount");
        if (mount.volume) {
            std::string opt = "type=volume,src=" + mount.host_path + ",destination=" +
                              mount.container_path;
            if (!mount.volume_subpath.empty()) {
                opt += (tool == ContainerTool::Docker ? ",volume-subpath=" : ",subpath=") +
                       mount.volume_subpath;
            }
            args.push_back(opt + (mount.read_only ? ",ro" : ""));
            continue;
        }
        args.push_back("type=bind,src=" + mount.host_path + ",destination=" +
                       mount.container_path + (mount.read_only ? ",ro" : ""));
    }

    args.push_back("--env");
    args.push_back("HOME=/tmp");
    for (const auto& [key, value] : spec.image.env) {
        const bool replaced = std::any_of(spec.env.begin(), spec.env.end(),
                                          [&key](const auto& entry) { return entry.first == key; });
        if (replaced) continue;
        args.push_back("--env");
        args.push_back(key + "=" + value);
    }
    for (const auto& [key, value] : spec.env) {
        args.push_back("--env");
        args.push_back(key + "=" + value);
    }

    // Docker publishes no port from an --internal network, and a shared
    // network namespace already exposes the port on lemond's own loopback.
    const bool shared_namespace = spec.network.rfind("container:", 0) == 0;
    if (tool == ContainerTool::Podman && !shared_namespace && spec.port > 0) {
        const std::string port = std::to_string(spec.port);
        args.push_back("-p");
        args.push_back("127.0.0.1:" + port + ":" + port);
    }

    args.push_back(spec.image.pinned_ref());
    args.insert(args.end(), spec.command.begin(), spec.command.end());
    return args;
}

CommandResult ContainerManager::invoke(const Info& info, const std::vector<std::string>& args,
                                       int timeout_seconds) {
    auto [executable, prefix] = invocation(info);
    prefix.insert(prefix.end(), args.begin(), args.end());
    return runner_(executable, prefix, timeout_seconds);
}

CommandResult ContainerManager::run(const std::vector<std::string>& args, int timeout_seconds) {
    const auto found = info();
    if (!found) {
        throw std::runtime_error("Neither podman nor docker is installed");
    }
    return invoke(*found, args, timeout_seconds);
}

bool ContainerManager::is_installed(ContainerTool tool) {
    const std::string name = tool == ContainerTool::Podman ? "podman" : "docker";
    switch (transport_) {
        case HostTransport::Native:
            return !find_executable_in_path(name).empty();
        case HostTransport::Toolbox: {
            const std::string spawn = find_executable_in_path("flatpak-spawn");
            if (spawn.empty()) return false;
            return runner_(spawn, {"--host", "sh", "-c", "command -v " + name},
                           kShortCommandTimeout)
                       .exit_code == 0;
        }
        case HostTransport::Container:
            return fs::exists(tool == ContainerTool::Podman ? kPodmanSocket : kDockerSocket);
        case HostTransport::Snap:
            return runner_("snapctl", {"is-connected", name}, kShortCommandTimeout).exit_code == 0;
    }
    return false;
}

std::optional<ContainerManager::Info> ContainerManager::select_tool() {
    for (const ContainerTool tool : {ContainerTool::Podman, ContainerTool::Docker}) {
        if (!is_installed(tool)) continue;
        Info found;
        found.tool = tool;
        found.executable = found.name();
        return found;
    }
    return std::nullopt;
}

std::optional<ContainerManager::Info> ContainerManager::info() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!info_) {
        info_ = select_tool();
        if (info_) {
            LOG(INFO, "Container") << "Using " << info_->name() << " ("
                                   << host_transport_name(transport_) << " host)" << std::endl;
        }
    }
    return info_;
}

bool ContainerManager::reachable(const Info& info) {
    return invoke(info, {"version"}, kShortCommandTimeout).exit_code == 0;
}

bool ContainerManager::started_by_service() const {
#ifdef _WIN32
    return false;
#else
    if (transport_ != HostTransport::Native) return false;
    const struct passwd* entry = ::getpwuid(::geteuid());
    return entry && std::string(entry->pw_name) == kServiceAccount;
#endif
}

std::string ContainerManager::install_command() const {
    std::string path = "/etc/os-release";
    if (transport_ == HostTransport::Snap) path = "/var/lib/snapd/hostfs/etc/os-release";
    if (transport_ == HostTransport::Toolbox) path = "/run/host/etc/os-release";
    return podman_install_command(read_file(path));
}

void ContainerManager::probe_self() {
    if (transport_ != HostTransport::Container) return;
    if (self_probed_ && !self_id_.empty()) return;
    self_probed_ = true;
    if (!info()) return;

    // The container's hostname is its short id unless the deployment
    // overrode it, in which case HOSTNAME is inspectable only if it is the
    // container name. Try both.
    std::vector<std::string> candidates;
    const std::string hostname = env_or_empty("HOSTNAME");
    if (!hostname.empty()) candidates.push_back(hostname);
#ifndef _WIN32
    char buffer[256] = {0};
    if (::gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0') {
        if (candidates.empty() || candidates.front() != buffer) candidates.push_back(buffer);
    }
#endif
    for (const auto& candidate : candidates) {
        const CommandResult id =
            run({"inspect", "--format", "{{.Id}}", candidate}, kShortCommandTimeout);
        if (id.exit_code != 0) continue;
        self_id_ = trim(id.output);
        const CommandResult mounts =
            run({"inspect", "--format", "{{json .Mounts}}", candidate}, kShortCommandTimeout);
        if (mounts.exit_code == 0) self_mounts_ = parse_self_mounts(mounts.output);
        break;
    }
}

std::string ContainerManager::self_container_id() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    probe_self();
    return self_id_;
}

ReadinessResult ContainerManager::check_readiness(const std::vector<std::string>& devices) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (readiness_ && readiness_devices_ == devices && now - readiness_time_ < kReadinessReuse) {
        return *readiness_;
    }

    const auto previous = info_;
    info_ = select_tool();
    if (info_ && (!previous || previous->tool != info_->tool)) {
        LOG(INFO, "Container") << "Using " << info_->name() << " ("
                               << host_transport_name(transport_) << " host)" << std::endl;
    }

    const bool service = started_by_service();
    const bool podman = !info_ || info_->tool == ContainerTool::Podman;
    ReadinessResult result;

    switch (transport_) {
        case HostTransport::Native:
            if (podman) {
                if (!info_) {
                    result = failure("podman is not on PATH", install_command(), false);
                } else if (service && !reachable(*info_)) {
                    result = failure("/run/lemonade-podman.sock does not answer",
                                     "sudo systemctl enable --now lemonade-podman.socket", false);
                } else if (!devices.empty() && !in_video_and_render()) {
                    result = service ? failure("lemonade is not in both video and render",
                                               "sudo usermod -aG video,render lemonade\n"
                                               "sudo systemctl restart lemond",
                                               true)
                                     : failure("The user's account is not in both video and render",
                                               "sudo usermod -aG video,render $USER\n"
                                               "Log out and back in",
                                               true);
                }
            } else if (!reachable(*info_)) {
                result = service ? failure("The Docker daemon refuses lemonade",
                                           "sudo usermod -aG docker lemonade\n"
                                           "sudo systemctl restart lemond",
                                           false)
                                 : failure("The Docker daemon refuses the user's account",
                                           "sudo usermod -aG docker $USER\n"
                                           "Log out and back in",
                                           false);
            }
            break;
        case HostTransport::Snap:
            if (podman) {
                if (!info_) {
                    result = failure("Neither the podman plug nor the docker plug is connected",
                                     install_command() +
                                         "\nsudo systemctl enable --now podman.socket\n"
                                         "sudo snap connect lemonade-server:podman :podman",
                                     false);
                } else if (!reachable(*info_)) {
                    result = failure("/run/podman/podman.sock does not answer",
                                     "sudo systemctl enable --now podman.socket", false);
                }
            } else if (!reachable(*info_)) {
                result = failure("The docker snap's daemon does not answer",
                                 "sudo snap start docker", false);
            }
            break;
        case HostTransport::Container:
            if (!info_) {
                result = failure(
                    "No socket is mounted at /run/podman/podman.sock or /var/run/docker.sock",
                    kDockerImageGuide, false);
            } else if (!reachable(*info_)) {
                result = failure(std::string(podman ? kPodmanSocket : kDockerSocket) +
                                     " does not answer",
                                 kDockerImageGuide, false);
            } else {
                self_probed_ = false;
                probe_self();
                if (self_id_.empty()) {
                    result = failure("lemond cannot inspect its own container through the socket",
                                     kDockerImageGuide, true);
                }
            }
            break;
        case HostTransport::Toolbox:
            if (podman) {
                if (!info_) {
                    result = failure("podman is not on the host's PATH", install_command(), false);
                }
            } else if (!reachable(*info_)) {
                result = failure("The Docker daemon refuses the user's host account",
                                 "sudo usermod -aG docker $USER\n"
                                 "Log out and back in",
                                 false);
            }
            break;
    }

    readiness_ = result;
    readiness_devices_ = devices;
    readiness_time_ = now;
    return result;
}

ContainerMount ContainerManager::host_mount(const std::string& visible_path,
                                            const std::string& container_path) {
    ContainerMount mount;
    mount.container_path = container_path;
    mount.host_path = visible_path;
    switch (transport_) {
        case HostTransport::Native:
        case HostTransport::Snap:
            return mount;
        case HostTransport::Toolbox: {
            const std::string kRunHost = "/run/host";
            if (visible_path.rfind(kRunHost + "/", 0) == 0) {
                mount.host_path = visible_path.substr(kRunHost.size());
            }
            return mount;
        }
        case HostTransport::Container: {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            probe_self();
            // The longest matching self-mount wins: its container path is the
            // prefix to strip and its host source the prefix to add.
            const ContainerMount* best = nullptr;
            for (const auto& self_mount : self_mounts_) {
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
                    "' is not inside any volume mounted into lemond's container, so podman or "
                    "docker on the host cannot mount it");
            }
            std::string suffix = visible_path.substr(best->container_path.size());
            if (!suffix.empty() && suffix.front() == '/') suffix.erase(0, 1);
            if (best->volume) {
                mount.volume = true;
                mount.host_path = best->host_path;
                mount.volume_subpath = suffix;
            } else {
                mount.host_path = suffix.empty() ? best->host_path : best->host_path + "/" + suffix;
            }
            return mount;
        }
    }
    return mount;
}

std::vector<std::string> ContainerManager::run_command(const ContainerRunSpec& spec) {
    const auto found = info();
    if (!found) {
        throw std::runtime_error("Neither podman nor docker is installed");
    }
    auto [executable, command] = invocation(*found);
    command.insert(command.begin(), executable);
    const std::vector<std::string> run_args = build_run_args(spec, found->tool);
    command.insert(command.end(), run_args.begin(), run_args.end());
    return command;
}

std::vector<std::string> ContainerManager::installed_digests(const std::string& repository) {
    if (!info()) return {};
    // `image inspect` resolves a bare repository to :latest, which does not
    // exist when the image was pulled by digest, so list the digests instead.
    const CommandResult result =
        run({"images", "--digests", "--format", "{{.Repository}}@{{.Digest}}", repository},
            kShortCommandTimeout);
    if (result.exit_code != 0) return {};
    return parse_repo_digests(result.output, repository);
}

bool ContainerManager::has_image_digest(const std::string& repository,
                                        const std::string& digest) {
    if (digest.empty()) return false;
    const auto digests = installed_digests(repository);
    return std::find(digests.begin(), digests.end(), digest) != digests.end();
}

std::string ContainerManager::installed_digest(const std::string& repository) {
    const auto digests = installed_digests(repository);
    return digests.empty() ? "" : digests.front();
}

void ContainerManager::pull(const ContainerImage& image, DownloadProgressCallback progress) {
    if (!image.valid()) {
        throw std::runtime_error("Cannot pull an image with no repository or digest");
    }
    const auto found = info();
    if (!found) {
        throw std::runtime_error("Neither podman nor docker is installed");
    }

    LOG(INFO, "Container") << "Pulling " << image.pinned_ref() << std::endl;
    DownloadProgress p;
    p.file = image.tagged_ref();
    p.file_index = 1;
    p.total_files = 1;
    if (progress) progress(p);

    const CommandResult result = invoke(*found, build_pull_args(image), kPullTimeout);
    if (result.exit_code != 0) {
        throw std::runtime_error("Failed to pull " + image.pinned_ref() + ": " +
                                 trim(result.output));
    }

    p.percent = 100;
    p.complete = true;
    if (progress) progress(p);
}

void ContainerManager::remove_image(const ContainerImage& image) {
    if (!image.valid() || !info()) return;
    const CommandResult result = run({"rmi", image.pinned_ref()}, kShortCommandTimeout);
    if (result.exit_code != 0) {
        LOG(WARNING, "Container") << "Could not remove image " << image.pinned_ref() << ": "
                                  << trim(result.output) << std::endl;
    }
}

void ContainerManager::stop(const std::string& name) {
    if (name.empty() || !info()) return;
    constexpr int kStopSeconds = 10;
    const CommandResult result = run(build_stop_args(name, kStopSeconds), kStopSeconds + 15);
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << "stop " << name << " returned " << result.exit_code << ": "
                                << trim(result.output) << std::endl;
    }
    // --rm detaches the container from its network asynchronously, so remove it
    // explicitly first or the network is still in use.
    remove_container(name);
    remove_network(name);
}

void ContainerManager::remove_container(const std::string& name) {
    if (name.empty() || !info()) return;
    run({"rm", "-f", name}, kShortCommandTimeout);
}

std::string ContainerManager::container_address(const std::string& name) {
    if (name.empty() || !info()) return "";
    const CommandResult result =
        run({"inspect", "--format", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}",
             name},
            kShortCommandTimeout);
    if (result.exit_code != 0) return "";
    return trim(result.output);
}

void ContainerManager::ensure_isolated_network(const std::string& name) {
    if (name.empty() || !info()) return;
    const CommandResult probe = run({"network", "inspect", name}, kShortCommandTimeout);
    if (probe.exit_code == 0) return;
    const CommandResult result =
        run({"network", "create", "--internal", "--label", kManagedLabel, name},
            kShortCommandTimeout);
    if (result.exit_code != 0) {
        throw std::runtime_error("Could not create the private network " + name + ": " +
                                 trim(result.output));
    }
}

void ContainerManager::remove_network(const std::string& name) {
    if (name.empty() || !info()) return;
    // A container started with --rm detaches from its network asynchronously
    // after it exits, so the first attempt can find the network still in use.
    for (int attempt = 0; attempt < 20; ++attempt) {
        const CommandResult result = run({"network", "rm", name}, kShortCommandTimeout);
        if (result.exit_code == 0) return;
        const std::string& output = result.output;
        if (output.find("not found") != std::string::npos ||
            output.find("no such network") != std::string::npos ||
            output.find("No such network") != std::string::npos) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    LOG(WARNING, "Container") << "Could not remove network " << name << std::endl;
}

int ContainerManager::sweep_managed_containers() {
    if (!info()) return 0;
    const std::string filter = std::string("label=") + kManagedLabel;
    int removed = 0;
    const CommandResult containers =
        run({"ps", "--all", "--filter", filter, "--format", "{{.Names}}"}, kShortCommandTimeout);
    if (containers.exit_code == 0) {
        for (const auto& name : split_lines(containers.output)) {
            LOG(INFO, "Container") << "Removing stale container " << name << std::endl;
            remove_container(name);
            ++removed;
        }
    }
    const CommandResult networks =
        run({"network", "ls", "--filter", filter, "--format", "{{.Name}}"}, kShortCommandTimeout);
    if (networks.exit_code == 0) {
        for (const auto& name : split_lines(networks.output)) remove_network(name);
    }
    return removed;
}

}  // namespace utils
}  // namespace lemon
