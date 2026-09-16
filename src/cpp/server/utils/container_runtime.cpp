#include "lemon/utils/container_runtime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

// Sanitize a recipe/variant token into something an engine accepts as a
// container name: [A-Za-z0-9][A-Za-z0-9_.-]*.
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
// True when the calling process holds `gid`, as a real or supplementary group.
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

const std::map<std::string, DeviceProfile>& profile_table() {
    // Mirrors the upstream catalog's runtime_profiles. Keeping the ids identical
    // means a catalog entry naming a profile resolves here without translation.
    // The engine's default seccomp profile applies unless a row says otherwise.
    static const std::map<std::string, DeviceProfile> table = [] {
        std::map<std::string, DeviceProfile> t;
        t["amd-rocm"] = DeviceProfile{
            "amd-rocm", {"/dev/dri", "/dev/kfd"}, {"video", "render"}, {}, {}, {}, false, false};
        t["amd-rocm-hipblaslt"] = DeviceProfile{
            "amd-rocm-hipblaslt", {"/dev/dri", "/dev/kfd"}, {"video", "render"},
            {}, {{"ROCBLAS_USE_HIPBLASLT", "1"}}, {}, false, false};
        t["amd-rocm-keep-groups"] = DeviceProfile{
            "amd-rocm-keep-groups", {"/dev/dri", "/dev/kfd"}, {"keep-groups"},
            {}, {}, {}, false, false};
        t["vulkan"] = DeviceProfile{
            "vulkan", {"/dev/dri"}, {"video", "render"}, {}, {}, {}, false, false};
        t["intel-level-zero"] = DeviceProfile{
            "intel-level-zero", {"/dev/dri"}, {"video", "render"}, {}, {}, {}, false, false};
        // DS4 memory-maps huge expert tensors across processes and reads its own
        // maps while streaming, which needs host IPC and SYS_PTRACE.
        t["ds4-rocm"] = DeviceProfile{
            "ds4-rocm", {"/dev/dri", "/dev/kfd"}, {"video", "render"},
            {}, {}, {"SYS_PTRACE"}, /*ipc_host=*/true,
            /*memlock_unlimited=*/false};
        return t;
    }();
    return table;
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

ContainerRuntime::ContainerRuntime(CommandRunner runner, std::optional<HostTransport> transport)
    : runner_(runner ? std::move(runner) : CommandRunner(default_runner)),
      transport_(transport ? *transport : detect_transport()) {}

ContainerRuntime& ContainerRuntime::global() {
    static ContainerRuntime instance;
    return instance;
}

HostTransport ContainerRuntime::detect_transport() {
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

std::pair<std::string, std::vector<std::string>> ContainerRuntime::engine_invocation(
    const ContainerEngine& engine) const {
    std::vector<std::string> prefix;
    std::string executable = engine.executable;
    if (transport_ == HostTransport::Toolbox) {
        const std::string spawn = find_executable_in_path("flatpak-spawn");
        if (!spawn.empty()) {
            prefix = {"--host", engine.name()};
            executable = spawn;
        }
    }
    if (engine.kind == ContainerEngineKind::Podman && !env_or_empty("CONTAINER_HOST").empty()) {
        prefix.push_back("--remote");
    }
    return {executable, prefix};
}

const char* ContainerRuntime::managed_label() { return kManagedLabel; }

std::string ContainerRuntime::container_name(const std::string& recipe,
                                             const std::string& variant) {
    std::string name = std::string(kManagedPrefix) + sanitize_name_token(recipe);
    if (!variant.empty()) name += "-" + sanitize_name_token(variant);
    return name;
}

const DeviceProfile& ContainerRuntime::device_profile(const std::string& id) {
    static const DeviceProfile empty;
    const auto& table = profile_table();
    auto it = table.find(id);
    return it == table.end() ? empty : it->second;
}

std::string ContainerRuntime::host_group_gid(const std::string& name) {
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

DeviceProfile ContainerRuntime::resolve_profile_groups(const DeviceProfile& profile) {
    DeviceProfile resolved = profile;
    resolved.groups.clear();
    for (const auto& group : profile.groups) {
        if (group == "keep-groups") {
            resolved.groups.push_back(group);
            continue;
        }
        const std::string gid = host_group_gid(group);
        if (!gid.empty()) resolved.groups.push_back(gid);
    }
    return resolved;
}

std::vector<std::string> ContainerRuntime::device_profile_ids() {
    std::vector<std::string> ids;
    for (const auto& [id, profile] : profile_table()) ids.push_back(id);
    return ids;
}

std::string ContainerRuntime::parse_engine_version(const std::string& output) {
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

std::vector<std::string> ContainerRuntime::parse_repo_digests(const std::string& output,
                                                              const std::string& repository) {
    // Engines print "<repository>@sha256:<hex>". Docker Hub images are commonly
    // stored without the "docker.io/" prefix, so match the bare form too rather
    // than requiring an exact hit.
    std::string bare = repository;
    const std::string kDockerIo = "docker.io/";
    if (bare.rfind(kDockerIo, 0) == 0) bare = bare.substr(kDockerIo.size());

    std::vector<std::string> digests;
    for (const auto& line : split_lines(output)) {
        const auto at = line.find('@');
        if (at == std::string::npos) continue;
        const std::string repo = line.substr(0, at);
        const std::string digest = line.substr(at + 1);
        // An image pulled by tag alone renders "<none>" here, and an engine with
        // nothing to show renders "<no value>". Neither is a digest.
        if (digest.rfind("sha256:", 0) != 0) continue;
        if (repo == repository || repo == bare) digests.push_back(digest);
    }
    return digests;
}

std::string ContainerRuntime::parse_repo_digest(const std::string& output,
                                                const std::string& repository) {
    const auto digests = parse_repo_digests(output, repository);
    return digests.empty() ? "" : digests.front();
}

std::string ContainerRuntime::rewrite_path(const std::vector<ContainerMount>& mounts,
                                           const std::string& host_path) {
    const ContainerMount* best = nullptr;
    size_t best_len = 0;
    for (const auto& mount : mounts) {
        if (mount.host_path.empty()) continue;
        if (host_path.rfind(mount.host_path, 0) != 0) continue;
        // Only a whole path component counts, so /data never matches /database.
        if (host_path.size() > mount.host_path.size() &&
            host_path[mount.host_path.size()] != '/' &&
            mount.host_path.back() != '/') {
            continue;
        }
        if (mount.host_path.size() >= best_len) {
            best = &mount;
            best_len = mount.host_path.size();
        }
    }
    if (!best) return "";
    std::string suffix = host_path.substr(best_len);
    if (!suffix.empty() && suffix.front() == '/') suffix.erase(0, 1);
    std::string target = best->container_path;
    if (!target.empty() && target.back() == '/') target.pop_back();
    return suffix.empty() ? target : target + "/" + suffix;
}

std::string ContainerRuntime::gfx_name_from_target_version(int target_version) {
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

std::string ContainerRuntime::pick_gpu_index(const std::vector<int>& target_versions,
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

std::string ContainerRuntime::kfd_gpu_index_for_arch(const std::string& arch) {
    std::error_code ec;
    if (!fs::is_directory(kKfdTopologyNodes, ec)) return "";
    std::vector<fs::path> nodes;
    for (const auto& entry : fs::directory_iterator(kKfdTopologyNodes, ec)) {
        nodes.push_back(entry.path());
    }
    std::sort(nodes.begin(), nodes.end(), [](const fs::path& a, const fs::path& b) {
        // Node directories are numbered; compare numerically so 10 sorts after 9.
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

std::vector<ContainerMount> ContainerRuntime::parse_self_mounts(const std::string& inspect_json) {
    std::vector<ContainerMount> mounts;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(inspect_json);
    } catch (...) {
        return mounts;
    }
    // podman/docker `inspect` print a one-element array for a single container;
    // `--format {{json .Mounts}}` prints the array of mounts directly.
    if (parsed.is_array() && !parsed.empty() && parsed[0].is_object() &&
        parsed[0].contains("Mounts")) {
        parsed = parsed[0]["Mounts"];
    }
    if (!parsed.is_array()) return mounts;
    for (const auto& entry : parsed) {
        if (!entry.is_object()) continue;
        std::string source = entry.value("Source", "");
        const std::string destination = entry.value("Destination", "");
        const std::string type = entry.value("Type", "");
        // A named volume's Source is the engine's private storage directory,
        // unreachable for a rootless engine. Its Name is what the engine
        // resolves in a mount source.
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

std::vector<std::string> ContainerRuntime::build_pull_args(const ContainerImageRef& ref) {
    return {"pull", ref.pinned_ref()};
}

std::vector<std::string> ContainerRuntime::build_stop_args(const std::string& name,
                                                           int timeout_seconds) {
    return {"stop", "--time", std::to_string(timeout_seconds), name};
}

std::vector<std::string> ContainerRuntime::build_run_args(const ContainerEngine& engine,
                                                          const ContainerRunSpec& spec) {
    std::vector<std::string> args;
    args.push_back("run");
    args.push_back("--rm");
    args.push_back("--init");
    args.push_back("--name");
    args.push_back(spec.name);
    args.push_back("--label");
    args.push_back(kManagedLabel);
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
    args.push_back("--network=" + (spec.network.empty() ? std::string("none") : spec.network));

    for (const auto& device : spec.profile.devices) {
        args.push_back("--device");
        args.push_back(device);
    }
    for (const auto& group : spec.profile.groups) {
        // keep-groups is a podman extension: it passes the invoking user's
        // supplementary groups into the container, which is how a rootless
        // podman reaches /dev/kfd without matching numeric gids. Docker has no
        // equivalent, so there the concrete group names are passed instead.
        if (group == "keep-groups" && engine.kind != ContainerEngineKind::Podman) {
            args.push_back("--group-add");
            args.push_back("video");
            args.push_back("--group-add");
            args.push_back("render");
            continue;
        }
        args.push_back("--group-add");
        args.push_back(group);
    }
    for (const auto& opt : spec.profile.security_opts) {
        args.push_back("--security-opt");
        args.push_back(opt);
    }
    for (const auto& cap : spec.profile.cap_add) {
        args.push_back("--cap-add");
        args.push_back(cap);
    }
    if (spec.profile.ipc_host) {
        args.push_back("--ipc=host");
    }
    if (spec.profile.memlock_unlimited) {
        args.push_back("--ulimit");
        args.push_back("memlock=-1:-1");
    }

    for (const auto& mount : spec.mounts) {
        args.push_back("--mount");
        if (mount.volume) {
            std::string opt = "type=volume,src=" + mount.host_path + ",destination=" +
                              mount.container_path;
            if (!mount.volume_subpath.empty()) {
                opt += (engine.kind == ContainerEngineKind::Docker ? ",volume-subpath="
                                                                    : ",subpath=") +
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
    for (const auto& [key, value] : spec.profile.env) {
        args.push_back("--env");
        args.push_back(key + "=" + value);
    }
    for (const auto& [key, value] : spec.env) {
        args.push_back("--env");
        args.push_back(key + "=" + value);
    }

    // A shared network namespace (container:<id>) already exposes the port on
    // lemond's own loopback, and both engines refuse -p there.
    const bool shared_namespace = spec.network.rfind("container:", 0) == 0;
    if (spec.publish_port && !shared_namespace && spec.host_port > 0 &&
        spec.container_port > 0) {
        // Loopback only: the container port is Lemonade's to proxy, never a
        // second externally reachable inference endpoint.
        args.push_back("-p");
        args.push_back("127.0.0.1:" + std::to_string(spec.host_port) + ":" +
                       std::to_string(spec.container_port));
    }

    if (!spec.workdir.empty()) {
        args.push_back("--workdir");
        args.push_back(spec.workdir);
    }
    if (!spec.entrypoint.empty()) {
        args.push_back("--entrypoint");
        args.push_back(spec.entrypoint);
    }

    args.push_back(spec.image);
    args.insert(args.end(), spec.command.begin(), spec.command.end());
    return args;
}

CommandResult ContainerRuntime::invoke(const ContainerEngine& engine,
                                       const std::vector<std::string>& args,
                                       int timeout_seconds) {
    auto [executable, prefix] = engine_invocation(engine);
    prefix.insert(prefix.end(), args.begin(), args.end());
    return runner_(executable, prefix, timeout_seconds);
}

CommandResult ContainerRuntime::run(const std::vector<std::string>& args, int timeout_seconds) {
    const auto& found = engine();
    if (!found) {
        throw std::runtime_error("No container runtime found (looked for podman, then docker)");
    }
    return invoke(*found, args, timeout_seconds);
}

std::optional<ContainerEngine> ContainerRuntime::probe(ContainerEngineKind kind,
                                                       const std::string& binary) {
    std::string path;
    if (transport_ == HostTransport::Toolbox) {
        // The engine lives on the host; inside the toolbox only flatpak-spawn
        // has to exist.
        if (find_executable_in_path("flatpak-spawn").empty()) return std::nullopt;
        path = binary;
    } else {
        path = find_executable_in_path(binary);
        if (path.empty()) return std::nullopt;
    }

    ContainerEngine found;
    found.kind = kind;
    found.executable = path;

    // `version` (not `--version`) round-trips to the daemon/socket, so a docker
    // CLI that cannot reach dockerd is reported as absent rather than failing
    // later inside load().
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
        found.version = parse_engine_version(fallback.output);
    }
    found.rootless = (kind == ContainerEngineKind::Podman);
    return found;
}

const std::optional<ContainerEngine>& ContainerRuntime::engine() {
    if (engine_probed_) return engine_;
    engine_probed_ = true;

    if (auto podman = probe(ContainerEngineKind::Podman, "podman")) {
        engine_ = podman;
    } else if (auto docker = probe(ContainerEngineKind::Docker, "docker")) {
        engine_ = docker;
    }
    if (engine_) {
        LOG(INFO, "Container") << "Using " << engine_->name() << " " << engine_->version
                               << " at " << engine_->executable << " ("
                               << host_transport_name(transport_) << " transport)" << std::endl;
    }
    return engine_;
}

void ContainerRuntime::reset_engine_cache() {
    engine_probed_ = false;
    engine_.reset();
    self_probed_ = false;
    self_id_.clear();
    self_mounts_.clear();
}

std::string ContainerRuntime::self_container_id() {
    if (transport_ != HostTransport::Container) return "";
    if (!self_probed_) self_mounts();
    return self_id_;
}

const std::vector<ContainerMount>& ContainerRuntime::self_mounts() {
    if (self_probed_ || transport_ != HostTransport::Container) return self_mounts_;
    self_probed_ = true;
    if (!engine()) return self_mounts_;

    // Both engines set the container's hostname to its short id unless the
    // deployment overrode it, in which case HOSTNAME still names something the
    // engine can inspect only if it is the container name. Try both.
    std::vector<std::string> candidates;
    const std::string hostname = env_or_empty("HOSTNAME");
    if (!hostname.empty()) candidates.push_back(hostname);
    char buffer[256] = {0};
#ifndef _WIN32
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
            << "lemond is running in a container but cannot inspect itself through the "
               "engine; model mounts cannot be translated to host paths" << std::endl;
    }
    return self_mounts_;
}

ReadinessResult ContainerRuntime::check_readiness(const DeviceProfile& profile) {
    if (!engine()) {
        if (transport_ == HostTransport::Container) {
            return {ContainerReadiness::EngineUnreachable,
                    "lemond is running in a container with no container engine socket "
                    "mounted, so it cannot launch toolbox containers.",
                    "engine-socket"};
        }
        if (transport_ == HostTransport::Snap) {
            return {ContainerReadiness::EngineUnreachable,
                    "The lemonade snap is not connected to the Docker snap's daemon.",
                    "snap-docker-plug"};
        }
        const std::string podman_path = find_executable_in_path("podman");
        const std::string docker_path = find_executable_in_path("docker");
        if (podman_path.empty() && docker_path.empty()) {
            return {ContainerReadiness::NoEngine,
                    "No container runtime found. Install podman (preferred) or docker.",
                    "no-container-runtime"};
        }
        return {ContainerReadiness::EngineUnreachable,
                "A container runtime is installed but not usable by this user "
                "(the daemon or socket did not answer).",
                "runtime-permissions"};
    }

    if (transport_ == HostTransport::Container) {
        // Device nodes and group membership inside lemond's own container say
        // nothing about the host the engine will launch on. What can be checked
        // here is that lemond can see itself through the engine, which is what
        // model mounts depend on.
        if (self_container_id().empty()) {
            return {ContainerReadiness::NoHostMountMapping,
                    "lemond cannot inspect its own container through the engine socket, so "
                    "model files cannot be mounted into toolbox containers.",
                    "engine-socket"};
        }
        return {};
    }
    if (transport_ == HostTransport::Toolbox) {
        // The toolbox shares the host's /dev and the user's groups, but the
        // engine's permission check happens on the host, so trust the engine.
        return {};
    }

    for (const auto& device : profile.devices) {
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
    const bool needs_real_groups =
        engine_ && engine_->kind == ContainerEngineKind::Podman && engine_->rootless;

    for (const auto& device : profile.devices) {
        // A profile entry may name a directory (/dev/dri), which both engines
        // accept as "every device under here". The directory itself is
        // world-readable and nobody-writable, so it answers the wrong question;
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

        // One usable node per directory is enough: /dev/dri holds both the
        // privileged card node and the unprivileged render node, and only the
        // render node is needed for compute.
        bool usable = false;
        for (const auto& node : nodes) {
            if (needs_real_groups) {
                struct stat info;
                if (::stat(node.c_str(), &info) != 0) continue;
                if (caller_in_group(info.st_gid)) {
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

std::vector<std::string> ContainerRuntime::installed_digests(const std::string& repository) {
    if (!engine()) return {};
    // `images --digests` is the portable query. `image inspect` cannot answer
    // this: it resolves a bare repository to :latest, which does not exist when
    // the image was pulled by digest, and .RepoDigests is not a field the
    // `images` formatter has at all.
    const CommandResult result =
        run({"images", "--digests", "--format", "{{.Repository}}@{{.Digest}}", repository},
            kShortCommandTimeout);
    if (result.exit_code != 0) return {};
    return parse_repo_digests(result.output, repository);
}

bool ContainerRuntime::has_image_digest(const std::string& repository,
                                        const std::string& digest) {
    if (digest.empty()) return false;
    const auto digests = installed_digests(repository);
    return std::find(digests.begin(), digests.end(), digest) != digests.end();
}

std::string ContainerRuntime::installed_digest(const std::string& repository) {
    const auto digests = installed_digests(repository);
    return digests.empty() ? "" : digests.front();
}

bool ContainerRuntime::image_present(const ContainerImageRef& ref) {
    if (!ref.valid()) return false;
    if (!ref.digest.empty()) {
        return has_image_digest(ref.repository, ref.digest);
    }
    if (!engine()) return false;
    const CommandResult result =
        run({"image", "inspect", ref.tagged_ref(), "--format", "{{.Id}}"}, kShortCommandTimeout);
    return result.exit_code == 0;
}

void ContainerRuntime::pull(const ContainerImageRef& ref, DownloadProgressCallback progress) {
    if (!ref.valid()) {
        throw std::runtime_error("Cannot pull an image with no repository");
    }
    const auto& found = engine();
    if (!found) {
        throw std::runtime_error("No container runtime found (looked for podman, then docker)");
    }

    LOG(INFO, "Container") << "Pulling " << ref.pinned_ref() << std::endl;
    if (progress) {
        DownloadProgress p;
        p.file = ref.tagged_ref();
        p.file_index = 1;
        p.total_files = 1;
        progress(p);
    }

    const CommandResult result = invoke(*found, build_pull_args(ref), kPullTimeout);
    if (result.exit_code != 0) {
        throw std::runtime_error("Failed to pull " + ref.pinned_ref() + ": " + trim(result.output));
    }

    if (progress) {
        DownloadProgress p;
        p.file = ref.tagged_ref();
        p.file_index = 1;
        p.total_files = 1;
        p.percent = 100;
        p.complete = true;
        progress(p);
    }
}

void ContainerRuntime::remove_image(const ContainerImageRef& ref) {
    if (!ref.valid() || !engine()) return;
    const CommandResult result = run({"rmi", ref.pinned_ref()}, kShortCommandTimeout);
    if (result.exit_code != 0) {
        LOG(WARNING, "Container") << "Could not remove image " << ref.pinned_ref() << ": "
                                  << trim(result.output) << std::endl;
    }
}

void ContainerRuntime::stop_container(const std::string& name, int timeout_seconds) {
    if (name.empty() || !engine()) return;
    const CommandResult result = run(build_stop_args(name, timeout_seconds), timeout_seconds + 15);
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << "stop " << name << " returned " << result.exit_code << ": "
                                << trim(result.output) << std::endl;
    }
}

void ContainerRuntime::remove_container(const std::string& name) {
    if (name.empty() || !engine()) return;
    run({"rm", "-f", name}, kShortCommandTimeout);
}

std::string ContainerRuntime::container_logs(const std::string& name, int tail) {
    if (name.empty() || !engine()) return "";
    const CommandResult result =
        run({"logs", "--tail", std::to_string(tail), name}, kShortCommandTimeout);
    return result.exit_code == 0 ? trim(result.output) : "";
}

std::string ContainerRuntime::container_address(const std::string& name) {
    if (name.empty() || !engine()) return "";
    const CommandResult result =
        run({"inspect", "--format", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}",
             name},
            kShortCommandTimeout);
    if (result.exit_code != 0) return "";
    return trim(result.output);
}

void ContainerRuntime::ensure_isolated_network(const std::string& name) {
    if (name.empty() || !engine()) return;
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

void ContainerRuntime::remove_network(const std::string& name) {
    if (name.empty() || !engine()) return;
    // A container started with --rm detaches from its network asynchronously
    // after it exits, so the first attempt can find the network still in use.
    for (int attempt = 0; attempt < 20; ++attempt) {
        const CommandResult result = run({"network", "rm", name}, kShortCommandTimeout);
        if (result.exit_code == 0) return;
        const std::string output = result.output;
        if (output.find("not found") != std::string::npos ||
            output.find("no such network") != std::string::npos ||
            output.find("No such network") != std::string::npos) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    LOG(WARNING, "Container") << "Could not remove network " << name << std::endl;
}

std::vector<std::string> ContainerRuntime::list_managed_networks() {
    std::vector<std::string> names;
    if (!engine()) return names;
    const CommandResult result =
        run({"network", "ls", "--filter", std::string("label=") + kManagedLabel, "--format",
             "{{.Name}}"},
            kShortCommandTimeout);
    if (result.exit_code != 0) return names;
    return split_lines(result.output);
}

std::vector<std::string> ContainerRuntime::list_managed_containers() {
    std::vector<std::string> names;
    if (!engine()) return names;
    const CommandResult result =
        run({"ps", "--all", "--filter", std::string("label=") + kManagedLabel, "--format",
             "{{.Names}}"},
            kShortCommandTimeout);
    if (result.exit_code != 0) return names;
    return split_lines(result.output);
}

int ContainerRuntime::sweep_managed_containers() {
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
