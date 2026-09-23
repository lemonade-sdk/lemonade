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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

namespace {

constexpr const char* kManagedPrefix = "lemonade-";
constexpr const char* kPrerequisitesUrl = "https://lemonade-server.ai/container_prerequisites.html";
constexpr const char* kManagedLabel = "ai.lemonade";
constexpr const char* kKfdTopologyNodes = "/sys/devices/virtual/kfd/kfd/topology/nodes";
constexpr int kShortCommandTimeout = 30;
constexpr int kPullTimeout = 3600;

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

#ifndef _WIN32
bool caller_in_group(gid_t gid) {
    if (::getgid() == gid || ::getegid() == gid) return true;
    const int count = ::getgroups(0, nullptr);
    if (count <= 0) return false;
    std::vector<gid_t> groups(static_cast<size_t>(count));
    if (::getgroups(count, groups.data()) < 0) return false;
    return std::find(groups.begin(), groups.end(), gid) != groups.end();
}
#endif

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

std::string container_prerequisites_url(const std::string& remediation_id) {
    return remediation_id.empty() ? std::string(kPrerequisitesUrl)
                                  : std::string(kPrerequisitesUrl) + "#" + remediation_id;
}

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
        const std::string spawn = find_executable_in_path("flatpak-spawn");
        if (!spawn.empty()) {
            prefix = {"--host", info.name()};
            executable = spawn;
        }
    }
    if (info.tool == ContainerTool::Podman && !env_or_empty("CONTAINER_HOST").empty()) {
        prefix.push_back("--remote");
    }
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

std::string ContainerManager::parse_version(const std::string& output) {
    // "podman version 5.3.1" / "Docker version 29.7.2, build a7dcaa6"
    for (const auto& line : split_lines(output)) {
        const auto pos = line.find("version ");
        if (pos == std::string::npos) continue;
        std::string rest = trim(line.substr(pos + 8));
        const auto cut = rest.find_first_of(", ");
        if (cut != std::string::npos) rest = rest.substr(0, cut);
        if (!rest.empty()) return rest;
    }
    return "";
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
    for (const auto& group : spec.groups) {
        args.push_back("--group-add");
        args.push_back(group);
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
    for (const auto* env : {&spec.image.env, &spec.env}) {
        for (const auto& [key, value] : *env) {
            args.push_back("--env");
            args.push_back(key + "=" + value);
        }
    }

    // A shared network namespace (container:<id>) already exposes the port on
    // lemond's own loopback, and both tools refuse -p there.
    const bool shared_namespace = spec.network.rfind("container:", 0) == 0;
    if (spec.publish_port && !shared_namespace && spec.host_port > 0 &&
        spec.container_port > 0) {
        // Loopback only: the container port is Lemonade's to proxy, never a
        // second externally reachable inference endpoint.
        args.push_back("-p");
        args.push_back("127.0.0.1:" + std::to_string(spec.host_port) + ":" +
                       std::to_string(spec.container_port));
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
    const auto& found = info();
    if (!found) {
        throw std::runtime_error("No container tool found (looked for podman, then docker)");
    }
    return invoke(*found, args, timeout_seconds);
}

std::optional<ContainerManager::Info> ContainerManager::probe(ContainerTool tool,
                                                              const std::string& binary) {
    std::string path;
    if (transport_ == HostTransport::Toolbox) {
        // The tool lives on the host; inside the toolbox only flatpak-spawn has
        // to exist.
        if (find_executable_in_path("flatpak-spawn").empty()) return std::nullopt;
        path = binary;
    } else {
        path = find_executable_in_path(binary);
        if (path.empty()) return std::nullopt;
    }

    Info found;
    found.tool = tool;
    found.executable = path;

    // `version` (not `--version`) round-trips to the daemon or socket, so a
    // docker CLI that cannot reach dockerd is reported as absent here rather
    // than failing later inside load().
    const CommandResult result =
        invoke(found, {"version", "--format", "{{.Client.Version}}"}, kShortCommandTimeout);
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << binary << " found at " << path
                                << " but is not usable: " << trim(result.output) << std::endl;
        return std::nullopt;
    }
    found.version = trim(result.output);
    if (found.version.empty()) {
        const CommandResult fallback = invoke(found, {"--version"}, kShortCommandTimeout);
        found.version = parse_version(fallback.output);
    }
    return found;
}

const std::optional<ContainerManager::Info>& ContainerManager::info() {
    if (info_probed_) return info_;
    info_probed_ = true;

    if (auto podman = probe(ContainerTool::Podman, "podman")) {
        info_ = podman;
    } else if (auto docker = probe(ContainerTool::Docker, "docker")) {
        info_ = docker;
    }
    if (info_) {
        LOG(INFO, "Container") << "Using " << info_->name() << " " << info_->version << " at "
                               << info_->executable << " (" << host_transport_name(transport_)
                               << " transport)" << std::endl;
    }
    return info_;
}

std::string ContainerManager::self_container_id() {
    if (transport_ != HostTransport::Container) return "";
    if (!self_probed_) self_mounts();
    return self_id_;
}

const std::vector<ContainerMount>& ContainerManager::self_mounts() {
    if (self_probed_ || transport_ != HostTransport::Container) return self_mounts_;
    self_probed_ = true;
    if (!info()) return self_mounts_;

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
    if (self_id_.empty()) {
        LOG(WARNING, "Container")
            << "lemond is running in a container but cannot inspect itself through podman or "
               "docker; model mounts cannot be translated to host paths" << std::endl;
    }
    return self_mounts_;
}

ReadinessResult ContainerManager::check_readiness(const std::vector<std::string>& devices) {
    if (!info()) {
        if (transport_ == HostTransport::Container) {
            return {ContainerReadiness::ContainerToolUnreachable,
                    "lemond is running in a container with no podman or docker socket "
                    "mounted, so it cannot launch toolbox containers.",
                    "engine-socket"};
        }
        if (transport_ == HostTransport::Snap) {
            return {ContainerReadiness::ContainerToolUnreachable,
                    "The lemonade snap is not connected to the Docker snap's daemon.",
                    "snap-docker-plug"};
        }
        if (find_executable_in_path("podman").empty() &&
            find_executable_in_path("docker").empty()) {
            return {ContainerReadiness::NoContainerTool,
                    "No container tool found. Install podman (preferred) or docker.",
                    "no-container-runtime"};
        }
        return {ContainerReadiness::ContainerToolUnreachable,
                "A container tool is installed but not usable by this user "
                "(the daemon or socket did not answer).",
                "runtime-permissions"};
    }

    if (transport_ == HostTransport::Container) {
        // The host's device nodes and groups are invisible from in here. What
        // can be checked is the self-inspection that model mounts rely on.
        if (self_container_id().empty()) {
            return {ContainerReadiness::NoHostMountMapping,
                    "lemond cannot inspect its own container through the podman or docker "
                    "socket, so model files cannot be mounted into toolbox containers.",
                    "engine-socket"};
        }
        return {};
    }
    if (transport_ == HostTransport::Toolbox) {
        // The toolbox shares the host's /dev and the user's groups, and the
        // permission check happens on the host, so trust the tool.
        return {};
    }

    for (const auto& device : devices) {
        if (fs::exists(device)) continue;
        if (device == "/dev/kfd") {
            return {ContainerReadiness::NoKfd,
                    "/dev/kfd is missing. The amdgpu kernel driver is not loaded, or this "
                    "kernel has no KFD support.",
                    "kfd-missing"};
        }
        return {ContainerReadiness::NoRenderNode,
                device + " is missing, so the GPU cannot be passed into a container.",
                "render-node-missing"};
    }

#ifndef _WIN32
    // logind grants the seat owner access to these nodes via a POSIX ACL, which
    // access() honors. An ACL is attached to a uid, and rootless podman runs in
    // a user namespace where that uid does not exist, so only real group
    // membership survives there.
    const bool needs_real_groups = info_->tool == ContainerTool::Podman;

    for (const auto& device : devices) {
        // /dev/dri is a directory that both tools accept as "every device under
        // here". The directory itself is world-readable and nobody-writable, so
        // the character devices inside it are what has to be openable.
        std::vector<std::string> nodes;
        if (fs::is_directory(device)) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(device, ec)) {
                if (fs::is_character_file(entry.path(), ec)) {
                    nodes.push_back(entry.path().string());
                }
            }
            if (nodes.empty()) {
                return {ContainerReadiness::NoRenderNode,
                        device + " contains no device nodes, so the GPU cannot be passed into "
                                 "a container.",
                        "render-node-missing"};
            }
        } else {
            nodes.push_back(device);
        }

        // /dev/dri holds both the privileged card node and the unprivileged
        // render node, and only the render node is needed for compute.
        bool usable = false;
        for (const auto& node : nodes) {
            if (needs_real_groups) {
                struct stat node_info;
                if (::stat(node.c_str(), &node_info) != 0) continue;
                if (caller_in_group(node_info.st_gid)) {
                    usable = true;
                    break;
                }
            } else if (::access(node.c_str(), R_OK | W_OK) == 0) {
                usable = true;
                break;
            }
        }
        if (usable) continue;

        if (needs_real_groups) {
            return {ContainerReadiness::NoGroupMembership,
                    "Rootless podman cannot pass access to " + device +
                        " through: this user is not a member of the group that owns it. "
                        "Add the user to the 'video' and 'render' groups and log back in.",
                    "group-membership"};
        }
        return {ContainerReadiness::NoGroupMembership,
                "This user cannot open " + device +
                    ". Add the user to the 'video' and 'render' groups and log back in.",
                "group-membership"};
    }
#endif

    return {};
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
            // The longest matching self-mount wins: its container path is the
            // prefix to strip and its host source the prefix to add.
            const ContainerMount* best = nullptr;
            for (const auto& self_mount : self_mounts()) {
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
                    "docker on the host cannot mount it. See " +
                    container_prerequisites_url("engine-socket"));
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

std::vector<std::string> ContainerManager::group_adds(const std::vector<std::string>& devices) {
    if (devices.empty()) return {};
    const auto& found = info();
    if (found && found->tool == ContainerTool::Podman) return {"keep-groups"};
    std::vector<std::string> gids;
    for (const char* group : {"video", "render"}) {
        const std::string gid = host_group_gid(group);
        if (!gid.empty()) gids.push_back(gid);
    }
    return gids;
}

std::vector<std::string> ContainerManager::run_command(const ContainerRunSpec& spec) {
    const auto& found = info();
    if (!found) {
        throw std::runtime_error("No container tool found. See " +
                                 container_prerequisites_url("no-container-runtime"));
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
        throw std::runtime_error("Cannot pull an image with no repository");
    }
    const auto& found = info();
    if (!found) {
        throw std::runtime_error("No container tool found (looked for podman, then docker)");
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
    stop_container(name);
    // --rm detaches the container from its network asynchronously, so remove it
    // explicitly first or the network is still in use.
    remove_container(name);
    remove_network(name);
}

void ContainerManager::stop_container(const std::string& name, int timeout_seconds) {
    if (name.empty() || !info()) return;
    const CommandResult result = run(build_stop_args(name, timeout_seconds), timeout_seconds + 15);
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << "stop " << name << " returned " << result.exit_code << ": "
                                << trim(result.output) << std::endl;
    }
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

std::vector<std::string> ContainerManager::list_managed_networks() {
    if (!info()) return {};
    const CommandResult result =
        run({"network", "ls", "--filter", std::string("label=") + kManagedLabel, "--format",
             "{{.Name}}"},
            kShortCommandTimeout);
    if (result.exit_code != 0) return {};
    return split_lines(result.output);
}

std::vector<std::string> ContainerManager::list_managed_containers() {
    if (!info()) return {};
    const CommandResult result =
        run({"ps", "--all", "--filter", std::string("label=") + kManagedLabel, "--format",
             "{{.Names}}"},
            kShortCommandTimeout);
    if (result.exit_code != 0) return {};
    return split_lines(result.output);
}

int ContainerManager::sweep_managed_containers() {
    if (!info()) return 0;
    int removed = 0;
    for (const auto& name : list_managed_containers()) {
        LOG(INFO, "Container") << "Removing stale container " << name << std::endl;
        remove_container(name);
        ++removed;
    }
    for (const auto& name : list_managed_networks()) {
        remove_network(name);
    }
    return removed;
}

}  // namespace utils
}  // namespace lemon
