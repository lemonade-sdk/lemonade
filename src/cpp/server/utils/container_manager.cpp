#include "lemon/utils/container_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <lemon/utils/aixlog.hpp>
#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#ifndef _WIN32
#include <grp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

namespace {

constexpr const char* kLabel = "ai.lemonade";
constexpr const char* kDockerSocket = "/var/run/docker.sock";
constexpr const char* kKfdTopologyNodes = "/sys/devices/virtual/kfd/kfd/topology/nodes";
constexpr int kStopSeconds = 10;
constexpr int kCommandTimeoutSeconds = 60;
constexpr int kPullTimeoutSeconds = 3 * 3600;

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        line = trim(line);
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

const char* tool_name(ContainerTool tool) {
    return tool == ContainerTool::Podman ? "podman" : "docker";
}

#ifndef _WIN32
// The group's ID, or nullopt when the host defines no such group. getgrnam_r,
// since setup checks run on concurrent request threads.
std::optional<gid_t> lookup_group(const std::string& group) {
    struct group entry = {};
    struct group* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getgrnam_r(group.c_str(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) {
        return std::nullopt;
    }
    return found->gr_gid;
}

bool process_in_group(const std::string& group) {
    const std::optional<gid_t> found = lookup_group(group);
    if (!found) return true;
    const gid_t gid = *found;
    if (::getgid() == gid || ::getegid() == gid) return true;
    const int count = ::getgroups(0, nullptr);
    if (count <= 0) return false;
    std::vector<gid_t> groups(static_cast<size_t>(count));
    if (::getgroups(count, groups.data()) < 0) return false;
    return std::find(groups.begin(), groups.end(), gid) != groups.end();
}

std::string host_group_id(const std::string& group) {
    const std::optional<gid_t> found = lookup_group(group);
    return found ? std::to_string(static_cast<unsigned long>(*found)) : "";
}

bool docker_socket_accepts_connection() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", kDockerSocket);
    const bool connected =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return connected;
}
#endif

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

CommandResult run_on_host(const std::vector<std::string>& argv, int timeout_seconds) {
    CommandResult result;
    const std::vector<std::string> args(argv.begin() + 1, argv.end());
    result.exit_code = ProcessManager::run_process_with_output(
        argv.front(), args,
        [&result](const std::string& line) {
            result.output += line;
            result.output += "\n";
            return true;
        },
        "", timeout_seconds, /*capture_stderr=*/true);
    return result;
}

// A container name accepts [A-Za-z0-9][A-Za-z0-9_.-]*.
std::string name_token(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (const char c : token) {
        const unsigned char uc = static_cast<unsigned char>(c);
        out.push_back((std::isalnum(uc) || c == '-' || c == '_' || c == '.') ? c : '-');
    }
    return out;
}

}  // namespace

ContainerHost ContainerHost::real() {
    ContainerHost host;
    host.on_path = [](const std::string& name) { return !find_executable_in_path(name).empty(); };
#ifdef _WIN32
    host.in_group = [](const std::string&) { return false; };
    host.group_id = [](const std::string&) { return std::string(); };
    host.docker_socket_accepts = []() { return false; };
#else
    host.in_group = process_in_group;
    host.group_id = host_group_id;
    host.docker_socket_accepts = docker_socket_accepts_connection;
#endif
    host.read_file = [](const std::string& path) { return utils::read_file(path); };
    host.run = run_on_host;
    return host;
}

ContainerManager::ContainerManager(ContainerHost host) : host_(std::move(host)) {}

ContainerManager& ContainerManager::global() {
    static ContainerManager instance;
    return instance;
}

std::optional<ContainerTool> ContainerManager::tool() const {
    if (host_.on_path("podman")) return ContainerTool::Podman;
    if (host_.on_path("docker")) return ContainerTool::Docker;
    return std::nullopt;
}

std::optional<SetupFailure> ContainerManager::check_setup() const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        return SetupFailure{"podman is not on PATH",
                            podman_install_command(host_.read_file("/etc/os-release"))};
    }
    if (*chosen == ContainerTool::Podman) {
        if (!host_.in_group("video") || !host_.in_group("render")) {
            return SetupFailure{"The user's account is not in both video and render",
                                "1. sudo usermod -aG video,render $USER\n"
                                "2. Log out and back in"};
        }
        return std::nullopt;
    }
    if (!host_.docker_socket_accepts()) {
        return SetupFailure{"The Docker daemon refuses the user's account",
                            "1. sudo usermod -aG docker $USER\n"
                            "2. Log out and back in"};
    }
    return std::nullopt;
}

std::optional<SetupFailure> ContainerManager::check_selinux() const {
    // Hosts without SELinux have no /sys/fs/selinux, so both reads come back empty.
    const bool enforcing = host_.read_file("/sys/fs/selinux/enforce").compare(0, 1, "1") == 0;
    const bool devices_allowed =
        host_.read_file("/sys/fs/selinux/booleans/container_use_devices").compare(0, 1, "0") != 0;
    if (enforcing && !devices_allowed) {
        return SetupFailure{"SELinux is enforcing and the container_use_devices boolean is off",
                            "sudo setsebool -P container_use_devices 1"};
    }
    return std::nullopt;
}

std::vector<std::string> ContainerManager::run_command(const ContainerRunSpec& spec) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    const bool podman = *chosen == ContainerTool::Podman;

    std::vector<std::string> argv = {
        tool_name(*chosen), "run", "--rm", "--init", "--name", spec.name,
        "--label", kLabel,
        "--label", std::string(kLabel) + ".recipe=" + spec.recipe,
        "--label", std::string(kLabel) + ".backend=" + spec.backend,
        "--label", std::string(kLabel) + ".model=" + spec.model,
        "--label", std::string(kLabel) + ".port=" + std::to_string(spec.port),
        "--cap-drop=all",
        "--security-opt=no-new-privileges",
        "--pull=never",
        "--network=" + spec.network,
    };

    for (const auto& device : spec.devices) {
        argv.push_back("--device");
        argv.push_back(device);
    }
    if (!spec.devices.empty()) {
        if (podman) {
            argv.push_back("--group-add");
            argv.push_back("keep-groups");
        } else {
            // Docker resolves a group name in the image's /etc/group, where it
            // may be missing or numbered differently from the device nodes.
            for (const char* group : {"video", "render"}) {
                const std::string gid = host_.group_id(group);
                if (gid.empty()) continue;
                argv.push_back("--group-add");
                argv.push_back(gid);
            }
        }
    }
    for (const auto& capability : spec.cap_add) {
        argv.push_back("--cap-add");
        argv.push_back(capability);
    }
    for (const auto& mount : spec.mounts) {
        // -v splits its argument on ':'.
        if (mount.source.find(':') != std::string::npos) {
            throw std::runtime_error("Cannot mount " + mount.source +
                                     " into a container: its path contains ':'");
        }
        argv.push_back("-v");
        argv.push_back(mount.source + ":" + mount.destination + ":ro,z");
    }
    argv.push_back("--env");
    argv.push_back("HOME=/tmp");
    for (const auto& [key, value] : spec.env) {
        argv.push_back("--env");
        argv.push_back(key + "=" + value);
    }
    // Docker publishes no port from an --internal network, so lemond reaches
    // a Docker container at its own address instead.
    if (podman) {
        const std::string port = std::to_string(spec.port);
        argv.push_back("-p");
        argv.push_back("127.0.0.1:" + port + ":" + port);
    }

    argv.push_back(spec.image);
    argv.insert(argv.end(), spec.command.begin(), spec.command.end());
    return argv;
}

CommandResult ContainerManager::invoke(const std::vector<std::string>& args,
                                       int timeout_seconds) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    std::vector<std::string> argv = {tool_name(*chosen)};
    argv.insert(argv.end(), args.begin(), args.end());
    return host_.run(argv, timeout_seconds);
}

bool ContainerManager::has_image(const std::string& image) const {
    if (!tool()) return false;
    return invoke({"image", "inspect", "--format", "{{.Id}}", image}, kCommandTimeoutSeconds)
               .exit_code == 0;
}

void ContainerManager::pull(const std::string& image,
                            const DownloadProgressCallback& progress) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    LOG(INFO, "Container") << "Pulling " << image << std::endl;

    DownloadProgress p;
    p.file = image;
    p.file_index = 1;
    p.total_files = 1;
    if (progress && !progress(p)) {
        throw std::runtime_error("Pull of " + image + " cancelled");
    }

    // Layer lines are the only progress either tool prints without a
    // terminal: Docker's "<layer>: Pulling fs layer" and "<layer>: Pull
    // complete", and Podman's "Copying blob <digest>" and "... done".
    std::set<std::string> layers;
    std::set<std::string> finished;
    std::string output;
    bool cancelled = false;
    const std::vector<std::string> args = {"pull", image};
    const int exit_code = ProcessManager::run_process_with_output(
        tool_name(*chosen), args,
        [&](const std::string& raw) {
            const std::string line = trim(raw);
            output += line + "\n";
            std::string layer;
            bool done = false;
            if (line.rfind("Copying blob ", 0) == 0) {
                std::istringstream words(line.substr(13));
                words >> layer;
                done = line.find(" done") != std::string::npos ||
                       line.find("skipped") != std::string::npos;
            } else if (const auto colon = line.find(": "); colon != std::string::npos) {
                layer = line.substr(0, colon);
                const std::string status = line.substr(colon + 2);
                if (status != "Pulling fs layer" && status != "Pull complete" &&
                    status != "Already exists") {
                    layer.clear();
                }
                done = status == "Pull complete" || status == "Already exists";
            }
            if (layer.empty()) return true;
            layers.insert(layer);
            if (done) finished.insert(layer);
            if (progress) {
                p.percent = static_cast<int>(finished.size() * 99 / layers.size());
                if (!progress(p)) {
                    cancelled = true;
                    return false;
                }
            }
            return true;
        },
        "", kPullTimeoutSeconds, /*capture_stderr=*/true);

    if (cancelled) {
        throw std::runtime_error("Pull of " + image + " cancelled");
    }
    if (exit_code != 0) {
        throw std::runtime_error("Failed to pull " + image + ": " + trim(output));
    }
    p.percent = 100;
    p.complete = true;
    if (progress) progress(p);
}

void ContainerManager::remove_image(const std::string& image) const {
    if (!tool()) return;
    const CommandResult result = invoke({"rmi", image}, kCommandTimeoutSeconds);
    if (result.exit_code != 0) {
        LOG(WARNING, "Container") << "Could not remove image " << image << ": "
                                  << trim(result.output) << std::endl;
    }
}

void ContainerManager::create_network(const std::string& name) const {
    remove_network(name);
    const CommandResult result =
        invoke({"network", "create", "--internal", "--label", kLabel, name}, kCommandTimeoutSeconds);
    if (result.exit_code != 0) {
        throw std::runtime_error("Could not create the network " + name + ": " +
                                 trim(result.output));
    }
}

void ContainerManager::remove_container(const std::string& name) const {
    if (!tool()) return;
    invoke({"rm", "-f", name}, kCommandTimeoutSeconds);
}

void ContainerManager::remove_network(const std::string& name) const {
    if (!tool()) return;
    // A container started with --rm leaves its network asynchronously, so the
    // network can still be in use for a moment after the container is gone.
    for (int attempt = 0; attempt < 20; ++attempt) {
        const CommandResult result = invoke({"network", "rm", name}, kCommandTimeoutSeconds);
        if (result.exit_code == 0) return;
        std::string output = result.output;
        std::transform(output.begin(), output.end(), output.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (output.find("no such network") != std::string::npos ||
            output.find("not found") != std::string::npos) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    LOG(WARNING, "Container") << "Could not remove network " << name << std::endl;
}

void ContainerManager::stop(const std::string& name) const {
    if (!tool()) return;
    const CommandResult result =
        invoke({"stop", "--time", std::to_string(kStopSeconds), name}, kStopSeconds + 30);
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << "stop " << name << " exited with " << result.exit_code << ": "
                                << trim(result.output) << std::endl;
    }
    remove_container(name);
    remove_network(name);
}

std::string ContainerManager::address(const std::string& name) const {
    if (!tool()) return "";
    const CommandResult result = invoke(
        {"inspect", "--format", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", name},
        kCommandTimeoutSeconds);
    return result.exit_code == 0 ? trim(result.output) : "";
}

void ContainerManager::sweep() const {
    if (!tool()) return;
    const std::string filter = std::string("label=") + kLabel;
    const CommandResult containers = invoke(
        {"ps", "--all", "--filter", filter, "--format", "{{.Names}}"}, kCommandTimeoutSeconds);
    if (containers.exit_code == 0) {
        for (const auto& name : lines_of(containers.output)) {
            LOG(INFO, "Container") << "Removing leftover container " << name << std::endl;
            remove_container(name);
        }
    }
    const CommandResult networks = invoke(
        {"network", "ls", "--filter", filter, "--format", "{{.Name}}"}, kCommandTimeoutSeconds);
    if (networks.exit_code == 0) {
        for (const auto& name : lines_of(networks.output)) {
            remove_network(name);
        }
    }
}

std::string ContainerManager::container_name(const std::string& recipe,
                                             const std::string& backend,
                                             const std::string& model) {
    return "lemonade-" + name_token(recipe) + "-" + name_token(backend) + "-" + name_token(model);
}

std::string ContainerManager::podman_install_command(const std::string& os_release) {
    std::string id;
    std::string id_like;
    for (const auto& line : lines_of(os_release)) {
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

bool ContainerManager::allowed_image(const std::string& image) {
    for (const std::string prefix : {"docker.io/kyuz0/", "ghcr.io/peonist-ai/"}) {
        if (image.size() > prefix.size() && image.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

std::string ContainerManager::gfx_name(int gfx_target_version) {
    if (gfx_target_version <= 0) return "";
    const int major = gfx_target_version / 10000;
    const int minor = (gfx_target_version / 100) % 100;
    const int stepping = gfx_target_version % 100;
    if (minor > 15 || stepping > 15) return "";
    static const char* hex = "0123456789abcdef";
    std::string name = "gfx" + std::to_string(major);
    name.push_back(hex[minor]);
    name.push_back(hex[stepping]);
    return name;
}

std::string ContainerManager::gpu_index(const std::vector<int>& gfx_target_versions,
                                        const std::string& arch) {
    if (arch.empty()) return "";
    int index = 0;
    for (const int version : gfx_target_versions) {
        if (version == 0) continue;
        if (gfx_name(version) == arch) return std::to_string(index);
        ++index;
    }
    return "";
}

std::string ContainerManager::kfd_gpu_index(const std::string& arch) {
    std::error_code ec;
    if (!fs::is_directory(kKfdTopologyNodes, ec)) return "";
    std::vector<std::pair<long, fs::path>> nodes;
    for (const auto& entry : fs::directory_iterator(kKfdTopologyNodes, ec)) {
        try {
            nodes.emplace_back(std::stol(entry.path().filename().string()), entry.path());
        } catch (...) {
        }
    }
    std::sort(nodes.begin(), nodes.end());
    std::vector<int> versions;
    for (const auto& [number, path] : nodes) {
        (void)number;
        std::ifstream properties(path / "properties");
        std::string key;
        long value = 0;
        int version = 0;
        while (properties >> key >> value) {
            if (key == "gfx_target_version") version = static_cast<int>(value);
        }
        versions.push_back(version);
    }
    return gpu_index(versions, arch);
}

}  // namespace utils
}  // namespace lemon
