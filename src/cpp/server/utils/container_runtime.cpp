#include "lemon/utils/container_runtime.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>

#include <lemon/utils/aixlog.hpp>
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
    static const std::map<std::string, DeviceProfile> table = [] {
        std::map<std::string, DeviceProfile> t;
        t["amd-rocm"] = DeviceProfile{
            "amd-rocm", {"/dev/dri", "/dev/kfd"}, {"video", "render"},
            {"seccomp=unconfined"}, {}, {}, false, false};
        t["amd-rocm-hipblaslt"] = DeviceProfile{
            "amd-rocm-hipblaslt", {"/dev/dri", "/dev/kfd"}, {"video", "render"},
            {"seccomp=unconfined"}, {{"ROCBLAS_USE_HIPBLASLT", "1"}}, {}, false, false};
        t["amd-rocm-keep-groups"] = DeviceProfile{
            "amd-rocm-keep-groups", {"/dev/dri", "/dev/kfd"}, {"keep-groups"},
            {"seccomp=unconfined"}, {}, {}, false, false};
        t["vulkan"] = DeviceProfile{
            "vulkan", {"/dev/dri"}, {"video"}, {"seccomp=unconfined"}, {}, {}, false, false};
        t["intel-level-zero"] = DeviceProfile{
            "intel-level-zero", {"/dev/dri"}, {"video", "render"},
            {"seccomp=unconfined"}, {}, {}, false, false};
        // DS4 memory-maps huge expert tensors across processes and reads its own
        // maps while streaming, which needs host IPC and SYS_PTRACE.
        t["ds4-rocm"] = DeviceProfile{
            "ds4-rocm", {"/dev/dri", "/dev/kfd"}, {"video", "render"},
            {"seccomp=unconfined"}, {}, {"SYS_PTRACE"}, /*ipc_host=*/true,
            /*memlock_unlimited=*/false};
        t["halogen-strix-halo"] = DeviceProfile{
            "halogen-strix-halo", {"/dev/kfd", "/dev/dri"}, {"video", "render"},
            {"seccomp=unconfined"}, {}, {}, /*ipc_host=*/true, /*memlock_unlimited=*/true};
        return t;
    }();
    return table;
}

}  // namespace

ContainerRuntime::ContainerRuntime(CommandRunner runner)
    : runner_(runner ? std::move(runner) : CommandRunner(default_runner)) {}

ContainerRuntime& ContainerRuntime::global() {
    static ContainerRuntime instance;
    return instance;
}

const char* ContainerRuntime::managed_name_prefix() { return kManagedPrefix; }

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
    args.push_back("--name");
    args.push_back(spec.name);

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
        args.push_back("-v");
        args.push_back(mount.host_path + ":" + mount.container_path +
                       (mount.read_only ? ":ro" : ""));
    }

    for (const auto& [key, value] : spec.profile.env) {
        args.push_back("--env");
        args.push_back(key + "=" + value);
    }
    for (const auto& [key, value] : spec.env) {
        args.push_back("--env");
        args.push_back(key + "=" + value);
    }

    if (spec.host_port > 0 && spec.container_port > 0) {
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

CommandResult ContainerRuntime::run(const std::vector<std::string>& args, int timeout_seconds) {
    const auto& found = engine();
    if (!found) {
        throw std::runtime_error("No container runtime found (looked for podman, then docker)");
    }
    return runner_(found->executable, args, timeout_seconds);
}

std::optional<ContainerEngine> ContainerRuntime::probe(ContainerEngineKind kind,
                                                       const std::string& binary) {
    const std::string path = find_executable_in_path(binary);
    if (path.empty()) return std::nullopt;

    // `version` (not `--version`) round-trips to the daemon/socket, so a docker
    // CLI that cannot reach dockerd is reported as absent rather than failing
    // later inside load().
    const CommandResult result = runner_(path, {"version", "--format", "{{.Client.Version}}"},
                                         kShortCommandTimeout);
    ContainerEngine found;
    found.kind = kind;
    found.executable = path;
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << binary << " found at " << path
                                << " but is not usable: " << trim(result.output) << std::endl;
        return std::nullopt;
    }
    found.version = trim(result.output);
    if (found.version.empty()) {
        const CommandResult fallback = runner_(path, {"--version"}, kShortCommandTimeout);
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
                               << " at " << engine_->executable << std::endl;
    }
    return engine_;
}

void ContainerRuntime::reset_engine_cache() {
    engine_probed_ = false;
    engine_.reset();
}

ReadinessResult ContainerRuntime::check_readiness(const DeviceProfile& profile) {
    if (!engine()) {
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

    const CommandResult result = runner_(found->executable, build_pull_args(ref), kPullTimeout);
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

std::vector<std::string> ContainerRuntime::list_containers(const std::string& name_prefix) {
    std::vector<std::string> names;
    if (!engine()) return names;
    const CommandResult result =
        run({"ps", "--all", "--format", "{{.Names}}"}, kShortCommandTimeout);
    if (result.exit_code != 0) return names;
    for (const auto& line : split_lines(result.output)) {
        if (name_prefix.empty() || line.rfind(name_prefix, 0) == 0) names.push_back(line);
    }
    return names;
}

int ContainerRuntime::sweep_containers(const std::string& name_prefix) {
    if (name_prefix.empty()) return 0;  // never sweep every container on the host
    int removed = 0;
    for (const auto& name : list_containers(name_prefix)) {
        LOG(INFO, "Container") << "Removing stale container " << name << std::endl;
        remove_container(name);
        ++removed;
    }
    return removed;
}

}  // namespace utils
}  // namespace lemon
