#include "lemon/utils/container_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "lemon/model_manager.h"
#include "lemon/utils/network_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <lemon/utils/aixlog.hpp>

#ifndef _WIN32
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

namespace {

constexpr const char* kLabel = "ai.lemonade";
constexpr const char* kServiceAccount = "lemonade";
constexpr int kStopSeconds = 10;
constexpr int kCommandTimeoutSeconds = 60;
constexpr int kDockerInfoTimeoutSeconds = 10;
constexpr int kPullTimeoutSeconds = 3 * 3600;
constexpr const char* kRelogin = "Log out and back in";
constexpr const char* kBackendNetwork = "lemonade-backends";
constexpr const char* kContainerDocs =
    "https://lemonade-server.ai/docs/guide/install/docker/#container-backends";

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

// Desktop distros often grant the logged-in account device access through
// ACLs rather than render membership, so this tests access itself.
bool account_opens_gpu_devices() {
    std::vector<fs::path> nodes = {"/dev/kfd"};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
        if (entry.path().filename().string().rfind("renderD", 0) == 0) {
            nodes.push_back(entry.path());
        }
    }
    for (const auto& node : nodes) {
        if (fs::exists(node, ec) && ::access(node.c_str(), R_OK | W_OK) != 0) return false;
    }
    return true;
}

std::string gpu_node_group() {
    std::vector<fs::path> nodes = {"/dev/kfd"};
    std::error_code ec;
    std::vector<fs::path> render;
    for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
        if (entry.path().filename().string().rfind("renderD", 0) == 0) {
            render.push_back(entry.path());
        }
    }
    std::sort(render.begin(), render.end());
    nodes.insert(nodes.end(), render.begin(), render.end());
    for (const auto& node : nodes) {
        struct stat info = {};
        if (::stat(node.c_str(), &info) == 0) {
            return std::to_string(static_cast<unsigned long>(info.st_gid));
        }
    }
    return "";
}

std::string process_account() {
    struct passwd entry = {};
    struct passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(::geteuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) {
        return "";
    }
    return found->pw_name;
}

std::string host_group_id(const std::string& group) {
    const std::optional<gid_t> found = lookup_group(group);
    return found ? std::to_string(static_cast<unsigned long>(*found)) : "";
}
#endif

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

CommandResult run_on_host(const std::vector<std::string>& argv, int timeout_seconds,
                          bool capture_stderr) {
    CommandResult result;
    if (argv.empty()) return result;
    const std::vector<std::string> args(argv.begin() + 1, argv.end());
    result.exit_code = ProcessManager::run_process_with_output(
        argv.front(), args,
        [&result](const std::string& line) {
            result.output += line;
            result.output += "\n";
            return true;
        },
        "", timeout_seconds, capture_stderr);
    return result;
}

// A container name accepts [A-Za-z0-9][A-Za-z0-9_.-]*. `_` is the escape
// character, so it is doubled and every other rejected byte becomes `_` plus
// two hex digits, which keeps distinct tokens distinct.
std::string name_token(const std::string& token) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(token.size());
    for (const char c : token) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '.') {
            out.push_back(c);
        } else if (c == '_') {
            out += "__";
        } else {
            out.push_back('_');
            out.push_back(kHex[uc >> 4]);
            out.push_back(kHex[uc & 0xf]);
        }
    }
    return out;
}

}  // namespace

ContainerHost ContainerHost::real() {
    ContainerHost host;
    host.on_path = [](const std::string& name) { return !find_executable_in_path(name).empty(); };
    host.exists = [](const std::string& path) {
        std::error_code ec;
        return fs::exists(path, ec);
    };
#ifdef _WIN32
    host.gpu_devices_accessible = []() { return false; };
    host.gpu_device_group = []() { return std::string(); };
    host.account = []() { return std::string(); };
    host.group_id = [](const std::string&) { return std::string(); };
#else
    host.gpu_devices_accessible = account_opens_gpu_devices;
    host.gpu_device_group = gpu_node_group;
    host.account = process_account;
    host.group_id = host_group_id;
#endif
    host.env = [](const std::string& name) {
        const char* value = std::getenv(name.c_str());
        return std::string(value ? value : "");
    };
#ifdef _WIN32
    host.is_root = []() { return false; };
#else
    host.is_root = []() { return ::geteuid() == 0; };
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
    // The image carries both clients; only the mounted socket says which host
    // tool there is.
    if (in_container()) {
        const auto given = [this](const std::string& url, const std::string& fallback) {
            if (url.empty()) return host_.exists(fallback);
            if (url.rfind("unix://", 0) != 0) return true;
            return host_.exists(url.substr(7));
        };
        if (host_.on_path("podman") && !host_.env("CONTAINER_HOST").empty() &&
            given(host_.env("CONTAINER_HOST"), "")) {
            return ContainerTool::Podman;
        }
        if (host_.on_path("docker") && given(host_.env("DOCKER_HOST"), "/var/run/docker.sock")) {
            return ContainerTool::Docker;
        }
    }
    if (host_.on_path("podman")) return ContainerTool::Podman;
    if (host_.on_path("docker")) return ContainerTool::Docker;
    return std::nullopt;
}

std::optional<bool> ContainerManager::podman_info_rootless() const {
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        if (podman_rootless_) return podman_rootless_;
    }
    const CommandResult info = host_.run(
        {"podman", "info", "--format",
         "{{.Host.Security.Rootless}} {{.Host.NetworkBackend}} {{.Host.OCIRuntime.Name}}"},
        kCommandTimeoutSeconds, /*capture_stderr=*/false);
    if (info.exit_code != 0) return std::nullopt;
    std::istringstream fields(info.output);
    std::string rootless_field;
    std::string backend;
    std::string runtime;
    fields >> rootless_field >> backend >> runtime;
    const bool rootless = rootless_field == "true";
    std::lock_guard<std::mutex> lock(podman_mode_mutex_);
    podman_network_backend_ = backend;
    podman_oci_runtime_ = runtime;
    // Asked again while rootless Podman lacks crun, so installing it takes
    // effect without restarting lemond.
    if (!needs_crun(rootless, runtime)) podman_rootless_ = rootless;
    return rootless;
}

bool ContainerManager::needs_crun(bool rootless, const std::string& runtime) {
    return rootless && !runtime.empty() && runtime != "crun";
}

bool ContainerManager::rootless_podman() const {
    return tool() == ContainerTool::Podman && podman_info_rootless().value_or(true);
}

bool ContainerManager::expect_rootless() const {
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        if (podman_rootless_) return *podman_rootless_;
    }
    if (host_.env("CONTAINER_HOST").empty()) return !host_.is_root();
    return own_podman_socket();
}

bool ContainerManager::own_podman_socket() const {
    // A socket in the account's own runtime directory is its podman.socket,
    // which runs rootless Podman outside lemond's sandbox.
    const std::string runtime_dir = host_.env("XDG_RUNTIME_DIR");
    return !runtime_dir.empty() &&
           host_.env("CONTAINER_HOST").rfind("unix://" + runtime_dir + "/", 0) == 0;
}

std::vector<SetupFailure> ContainerManager::check_host() const {
    std::vector<SetupFailure> failures;
    // In a container, the image's own account, groups and os-release say
    // nothing about the host, so the one check is that the host's tool, reached
    // through a mounted socket, finds lemond's container.
    const bool contained = in_container();
    if (contained && own_container().empty()) {
        failures.push_back(
            {"own_container",
             "lemond runs in a container that no Podman or Docker socket mounted into it can find",
             {std::string("Start lemond's container with the host's Podman or Docker socket mounted, as ") +
              kContainerDocs + " shows"},
             ""});
        return failures;
    }
    if (contained && backend_network().empty()) {
        std::vector<std::string> steps;
        std::string usual = "podman";
        if (tool() == ContainerTool::Docker) {
            // Docker refuses its default bridge alongside a user-defined network.
            usual = "lemonade";
            steps = {std::string("docker network create --internal ") + kBackendNetwork,
                     "docker network create " + usual};
        } else {
            // lemond connects by address, and rootless Podman's DNS server needs
            // a systemd user manager (see create_network).
            steps = {std::string("podman network create --internal --disable-dns ") + kBackendNetwork};
        }
        steps.push_back("Start lemond's container with --network " + usual + " --network " +
                        kBackendNetwork + ", as " + kContainerDocs + " shows");
        failures.push_back(
            {"backend_network",
             std::string("lemond's container is not on an internal network named ") + kBackendNetwork,
             steps, ""});
        return failures;
    }
    // Docker and rootful Podman add the render group by number (see run_command).
    if (contained && !rootless_podman() && render_group_id().empty()) {
        failures.push_back(
            {"render_group", "lemond cannot tell the host's render group ID from inside its container",
             {"Start lemond's container with -e LEMONADE_RENDER_GID=$(getent group render | cut -d: -f3)"},
             ""});
        return failures;
    }
    const bool service = host_.account() == kServiceAccount;
    const std::string member = service ? kServiceAccount : "$USER";
    const std::string apply = service ? "sudo systemctl restart lemond" : kRelogin;
    const std::optional<ContainerTool> chosen = tool();
    if (chosen == ContainerTool::Docker) {
        const CommandResult info = host_.run(
            {"docker", "info", "--format", "{{.OperatingSystem}}|{{json .SecurityOptions}}"},
            kDockerInfoTimeoutSeconds, /*capture_stderr=*/false);
        const std::string install = install_command(host_.read_file("/etc/os-release"), "podman", "Podman");
        if (info.exit_code != 0) {
            failures.push_back({"docker_access", "The Docker daemon refuses the account",
                                {"sudo usermod -aG docker " + member}, apply});
        } else if (info.output.find("Docker Desktop") != std::string::npos) {
            // Its containers run in a VM that has no access to the host GPU.
            failures.push_back(
                {"docker_desktop", "Docker Desktop cannot give containers the GPU", {install}, ""});
        } else if (info.output.find("name=rootless") != std::string::npos) {
            // Rootless Docker can neither publish a port from an --internal
            // network nor expose the container's address to lemond.
            failures.push_back(
                {"docker_rootless", "Rootless Docker cannot run container backends", {install}, ""});
        }
        return failures;
    }
    // With neither tool, the user is told to install Podman, so its group
    // check applies as well.
    if (!chosen) {
        failures.push_back({"container_tool", "podman is not on PATH",
                            {install_command(host_.read_file("/etc/os-release"), "podman", "Podman")}, ""});
    }
    if (!contained && expect_rootless() && !host_.gpu_devices_accessible()) {
        failures.push_back({"device_groups", "Account cannot open the GPU devices",
                            {"sudo usermod -aG render " + member}, apply});
    }
    return failures;
}

std::optional<SetupFailure> ContainerManager::check_podman() const {
    if (tool() != ContainerTool::Podman) return std::nullopt;
    // Only local rootless Podman maps container IDs onto lemonade's own ranges;
    // a root or remote Podman behind CONTAINER_HOST uses its own.
    if (host_.account() == kServiceAccount && !in_container() && expect_rootless() &&
        host_.on_path("getsubids")) {
        const auto has_range = [this](const std::vector<std::string>& argv) {
            const CommandResult result =
                host_.run(argv, kCommandTimeoutSeconds, /*capture_stderr=*/false);
            return result.exit_code == 0 && !trim(result.output).empty();
        };
        if (!has_range({"getsubids", kServiceAccount}) ||
            !has_range({"getsubids", "-g", kServiceAccount})) {
            return SetupFailure{"subordinate_ids",
                                "getsubids lemonade or getsubids -g lemonade prints no range",
                                {"Ask the host's administrator to allocate 65,536 subordinate "
                                 "UIDs and GIDs to lemonade"},
                                "sudo systemctl restart lemond"};
        }
    }
    const std::optional<bool> rootless = podman_info_rootless();
    if (!rootless) {
        // A shell's `podman info` reaches local Podman, not the CONTAINER_HOST
        // lemond was given.
        const std::string container_host = host_.env("CONTAINER_HOST");
        std::string step = "Run `podman info` and fix the error it prints";
        if (own_podman_socket()) {
            step = "systemctl --user enable --now podman.socket";
        } else if (!container_host.empty()) {
            step = "Run `podman info` with CONTAINER_HOST=" + container_host +
                   " and fix the error it prints";
        }
        return SetupFailure{"podman_info", "`podman info` fails", {step}, ""};
    }
    std::string runtime;
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        runtime = podman_oci_runtime_;
    }
    // Rootless containers reach the GPU through the account's render group,
    // which only crun keeps (keep-groups); runc resets supplementary groups.
    if (needs_crun(*rootless, runtime)) {
        return SetupFailure{
            "oci_runtime",
            "Rootless Podman runs containers with " + runtime + ", which cannot give them the GPU",
            {install_command(host_.read_file("/etc/os-release"), "crun", "crun")}, ""};
    }
    // Podman 4's CNI backend gives an internal network no gateway on the host,
    // so rootful lemond cannot reach the container's address (see run_command).
    std::string backend;
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        backend = podman_network_backend_;
    }
    if (!*rootless && backend == "cni") {
        const bool service = host_.account() == kServiceAccount;
        std::string step = "Run lemond as a regular account to use rootless Podman";
        std::string apply;
        if (!host_.env("CONTAINER_HOST").empty()) {
            step = service ? "Remove CONTAINER_HOST from /etc/default/lemond to use rootless Podman"
                           : "Unset CONTAINER_HOST to use rootless Podman";
            if (service) apply = "sudo systemctl restart lemond";
        }
        return SetupFailure{
            "rootful_cni",
            "Rootful Podman's CNI network backend gives lemond no route to its containers",
            {step}, apply};
    }
    return std::nullopt;
}

std::optional<SetupFailure> ContainerManager::check_selinux() const {
    // Hosts without SELinux have no /sys/fs/selinux, so both reads come back empty.
    const bool enforcing = host_.read_file("/sys/fs/selinux/enforce").compare(0, 1, "1") == 0;
    const bool devices_allowed =
        host_.read_file("/sys/fs/selinux/booleans/container_use_devices").compare(0, 1, "0") != 0;
    if (enforcing && !devices_allowed) {
        return SetupFailure{"selinux",
                            "SELinux is enforcing and the container_use_devices boolean is off",
                            {"sudo setsebool -P container_use_devices 1"}, ""};
    }
    return std::nullopt;
}

std::vector<std::string> SetupReport::ids() const {
    std::vector<std::string> ids;
    for (const auto& failure : failures) ids.push_back(failure.id);
    return ids;
}

std::string SetupReport::message() const {
    std::string text;
    for (const auto& failure : failures) {
        if (!text.empty()) text += '\n';
        text += failure.message;
    }
    return text;
}

std::string SetupReport::action() const {
    std::vector<std::string> steps;
    std::vector<std::string> applies;
    for (const auto& failure : failures) {
        steps.insert(steps.end(), failure.steps.begin(), failure.steps.end());
        if (!failure.apply.empty() &&
            std::find(applies.begin(), applies.end(), failure.apply) == applies.end()) {
            applies.push_back(failure.apply);
        }
    }
    steps.insert(steps.end(), applies.begin(), applies.end());
    if (steps.size() == 1) return steps.front();
    std::string text;
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i > 0) text += '\n';
        text += std::to_string(i + 1) + ". " + steps[i];
    }
    return text;
}

std::string SetupReport::text() const {
    std::string text;
    for (const auto& line : lines_of(message())) text += "\n  - " + line;
    text += "\nTo fix it:";
    for (const auto& line : lines_of(action())) text += "\n  " + line;
    return text;
}

std::vector<std::string> ContainerManager::run_command(const ContainerRunSpec& spec) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    const bool rootless = rootless_podman();

    std::vector<std::string> argv = {
        tool_name(*chosen), "run", "--rm", "--name", spec.name,
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
    // The namespace's PID 1 reaps for a container that joins it, and Podman
    // refuses --init there.
    if (spec.pid.empty()) {
        argv.insert(argv.begin() + 3, "--init");
    } else {
        argv.push_back("--pid=" + spec.pid);
    }

    for (const auto& device : spec.devices) {
        argv.push_back("--device");
        argv.push_back(device);
    }
    if (!spec.devices.empty()) {
        if (rootless && host_.env("CONTAINER_HOST").empty()) {
            argv.push_back("--group-add");
            argv.push_back("keep-groups");
        } else if (rootless) {
            // The Podman API rejects keep-groups; this is the crun annotation it sets.
            argv.push_back("--annotation");
            argv.push_back("run.oci.keep_original_groups=1");
        } else {
            // Docker resolves a group name in the image's /etc/group, where it
            // may be missing or numbered differently from the device nodes.
            const std::string gid = render_group_id();
            if (!gid.empty()) {
                argv.push_back("--group-add");
                argv.push_back(gid);
            }
        }
    }
    for (const auto& capability : spec.cap_add) {
        argv.push_back("--cap-add");
        argv.push_back(capability);
    }
    const bool contained = !own_container().empty();
    for (const auto& mount : spec.mounts) {
        const std::string source = contained ? host_path(mount.source) : mount.source;
        // -v splits its argument on ':'.
        if (source.find(':') != std::string::npos) {
            throw std::runtime_error("Cannot mount " + source +
                                     " into a container: its path contains ':'");
        }
        argv.push_back("-v");
        argv.push_back(source + ":" + mount.destination + ":ro,z");
    }
    argv.push_back("--env");
    argv.push_back("HOME=/tmp");
    for (const auto& [key, value] : spec.env) {
        argv.push_back("--env");
        argv.push_back(key + "=" + value);
    }
    // lemond reaches a rootless container through this published port and a
    // rootful one at the container's address on its network. From its own
    // container, lemond reaches every backend at its address.
    if (rootless && !contained) {
        const std::string port = std::to_string(spec.port);
        argv.push_back("-p");
        argv.push_back("127.0.0.1:" + port + ":" + port);
    }

    argv.push_back(spec.image);
    argv.insert(argv.end(), spec.command.begin(), spec.command.end());
    return argv;
}

CommandResult ContainerManager::invoke(const std::vector<std::string>& args,
                                       int timeout_seconds, bool capture_stderr) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    std::vector<std::string> argv = {tool_name(*chosen)};
    argv.insert(argv.end(), args.begin(), args.end());
    return host_.run(argv, timeout_seconds, capture_stderr);
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
    std::vector<std::string> args = {"network", "create", "--internal", "--label", kLabel};
    // Rootless Podman starts the network's DNS server through the account's
    // systemd user manager, which lemond.service's account does not have.
    // Rootful Podman keeps DNS because netavark gives an internal network
    // without it no gateway (see run_command).
    if (rootless_podman()) args.push_back("--disable-dns");
    args.push_back(name);
    const CommandResult result = invoke(args, kCommandTimeoutSeconds);
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
        {"inspect", "--type", "container", "--format",
         "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", name},
        kCommandTimeoutSeconds, /*capture_stderr=*/false);
    return result.exit_code == 0 ? trim(result.output) : "";
}

bool ContainerManager::in_container() const {
    return host_.exists && (host_.exists("/run/.containerenv") || host_.exists("/.dockerenv"));
}

std::string ContainerManager::render_group_id() const {
    if (!in_container()) return host_.group_id("render");
    const std::string given = trim(host_.env("LEMONADE_RENDER_GID"));
    if (!given.empty()) return given;
    return host_.gpu_device_group ? host_.gpu_device_group() : "";
}

std::vector<std::string> ContainerManager::own_container_candidates() const {
    std::vector<std::string> candidates;
    // The container's /etc/hostname and friends are bind mounts out of the
    // tool's per-container directory, .../containers/<id>/ under Docker and
    // .../overlay-containers/<id>/userdata/ under Podman, which holds even when
    // --hostname replaces the default short ID.
    static const std::regex kContainerDir("containers/([0-9a-f]{64})/");
    const std::string mountinfo = host_.read_file("/proc/self/mountinfo");
    for (std::sregex_iterator it(mountinfo.begin(), mountinfo.end(), kContainerDir), end;
         it != end; ++it) {
        const std::string id = (*it)[1].str();
        if (std::find(candidates.begin(), candidates.end(), id) == candidates.end()) {
            candidates.push_back(id);
        }
    }
    const std::string hostname = trim(host_.read_file("/etc/hostname"));
    if (!hostname.empty()) candidates.push_back(hostname);
    return candidates;
}

std::string ContainerManager::own_container() const {
    if (!in_container()) return "";
    std::lock_guard<std::mutex> lock(own_container_mutex_);
    if (!own_container_.empty()) return own_container_;
    if (!tool()) return "";
    for (const auto& candidate : own_container_candidates()) {
        const CommandResult result =
            invoke({"inspect", "--type", "container", "--format",
                    "{{.Id}}|{{json .Mounts}}|{{json .NetworkSettings.Networks}}", candidate},
                   kCommandTimeoutSeconds, /*capture_stderr=*/false);
        if (result.exit_code != 0) continue;
        const std::string output = trim(result.output);
        const auto bar = output.find('|');
        // Under pasta, Podman reports the networks as null rather than {}.
        const auto bar2 = output.rfind('|');
        if (bar == std::string::npos || bar2 == bar) continue;
        std::vector<OwnMount> mounts;
        const auto parsed = nlohmann::json::parse(output.substr(bar + 1, bar2 - bar - 1), nullptr, false);
        if (parsed.is_array()) {
            for (const auto& mount : parsed) {
                const std::string source = mount.value("Source", "");
                const std::string destination = mount.value("Destination", "");
                if (!source.empty() && !destination.empty()) {
                    mounts.push_back({destination, source});
                }
            }
        }
        // Which of lemond's networks are internal, and lemond's addresses on them.
        std::map<std::string, std::vector<std::string>> addresses;
        const auto networks = nlohmann::json::parse(output.substr(bar2 + 1), nullptr, false);
        if (networks.is_object()) {
            for (const auto& [name, settings] : networks.items()) {
                auto& list = addresses[name];
                for (const char* key : {"IPAddress", "GlobalIPv6Address"}) {
                    const std::string address =
                        settings.is_object() ? settings.value(key, "") : std::string();
                    if (!address.empty()) list.push_back(address);
                }
            }
        }
        std::string backend_network;
        if (!addresses.empty()) {
            std::vector<std::string> args = {"network", "inspect", "--format",
                                             "{{.Name}} {{.Internal}}"};
            for (const auto& [name, _] : addresses) args.push_back(name);
            const CommandResult inspected =
                invoke(args, kCommandTimeoutSeconds, /*capture_stderr=*/false);
            for (const auto& line : lines_of(inspected.output)) {
                std::istringstream words(line);
                std::string name;
                std::string internal;
                words >> name >> internal;
                if (internal != "true" || !addresses.count(name)) continue;
                if (name == kBackendNetwork) backend_network = name;
            }
        }
        own_container_ = output.substr(0, bar);
        own_mounts_ = std::move(mounts);
        backend_network_ = backend_network;
        set_refused_local_addresses(backend_network.empty() ? std::vector<std::string>()
                                                            : addresses[backend_network]);
        LOG(INFO, "Container") << "lemond runs in container " << own_container_
                               << "; container backends run beside it on "
                               << (backend_network.empty() ? "no internal network" : backend_network)
                               << std::endl;
        return own_container_;
    }
    return "";
}

std::string ContainerManager::backend_network() const {
    if (own_container().empty()) return "";
    std::lock_guard<std::mutex> lock(own_container_mutex_);
    return backend_network_;
}

std::string ContainerManager::host_path(const std::string& path) const {
    own_container();
    std::lock_guard<std::mutex> lock(own_container_mutex_);
    const OwnMount* best = nullptr;
    for (const auto& mount : own_mounts_) {
        const std::string& dest = mount.destination;
        const bool holds = path == dest || dest == "/" ||
                           (path.size() > dest.size() && path.compare(0, dest.size(), dest) == 0 &&
                            path[dest.size()] == '/');
        if (holds && (!best || dest.size() > best->destination.size())) best = &mount;
    }
    if (!best) {
        throw std::runtime_error("Cannot mount " + path +
                                 " into a container backend: it is not on a volume or bind "
                                 "mount of lemond's container");
    }
    if (best->destination == "/") return best->source + path;
    return best->source + path.substr(best->destination.size());
}

std::string ContainerManager::container_name(const std::string& instance,
                                             const std::string& recipe,
                                             const std::string& backend,
                                             const std::string& model) {
    return "lemonade-" + name_token(instance) + "-" + name_token(recipe) + "-" +
           name_token(backend) + "-" + name_token(model);
}

std::string ContainerManager::install_command(const std::string& os_release,
                                              const std::string& package,
                                              const std::string& name) {
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
        if (candidate == "debian") return "sudo apt install " + package;
        if (candidate == "fedora") return "sudo dnf install " + package;
        if (candidate == "arch") return "sudo pacman -S " + package;
    }
    return "Install " + name + " with the host's package manager";
}

bool ContainerManager::allowed_image(const std::string& image) {
    for (const std::string prefix : {"docker.io/kyuz0/", "ghcr.io/peonist-ai/"}) {
        if (image.size() > prefix.size() && image.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<ContainerMount> ContainerManager::model_mounts(const std::string& host_path,
                                                           const std::string& destination) {
    std::error_code ec;
    const fs::path source = fs::canonical(fs::path(host_path), ec);
    if (ec) {
        throw std::runtime_error("Model file '" + host_path + "' does not exist");
    }
    if (!fs::is_directory(source, ec)) {
        return {{source.string(), destination}};
    }
    std::vector<ContainerMount> mounts;
    const fs::path named = fs::path(host_path).lexically_normal();
    for (const auto& entry : fs::recursive_directory_iterator(named, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path file = fs::canonical(entry.path(), ec);
        if (ec) {
            throw std::runtime_error("Model file '" + entry.path().string() +
                                     "' links to a file that does not exist");
        }
        const std::string relative = entry.path().lexically_relative(named).generic_string();
        mounts.push_back({file.string(), destination + "/" + relative});
    }
    if (ec) {
        throw std::runtime_error("Cannot read model directory '" + host_path + "': " +
                                 ec.message());
    }
    std::sort(mounts.begin(), mounts.end(),
              [](const ContainerMount& a, const ContainerMount& b) {
                  return a.destination < b.destination;
              });
    return mounts;
}

}  // namespace utils
}  // namespace lemon
