#include "lemon/utils/container_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <lemon/utils/aixlog.hpp>

#ifndef _WIN32
#include <grp.h>
#include <pwd.h>
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
// ACLs rather than video/render membership, so this tests access itself.
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
#ifdef _WIN32
    host.gpu_devices_accessible = []() { return false; };
    host.account = []() { return std::string(); };
    host.group_id = [](const std::string&) { return std::string(); };
#else
    host.gpu_devices_accessible = account_opens_gpu_devices;
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
        {"podman", "info", "--format", "{{.Host.Security.Rootless}} {{.Host.NetworkBackend}}"},
        kCommandTimeoutSeconds, /*capture_stderr=*/false);
    if (info.exit_code != 0) return std::nullopt;
    std::istringstream fields(info.output);
    std::string rootless_field;
    std::string backend;
    fields >> rootless_field >> backend;
    const bool rootless = rootless_field == "true";
    std::lock_guard<std::mutex> lock(podman_mode_mutex_);
    podman_rootless_ = rootless;
    podman_network_backend_ = backend;
    return rootless;
}

bool ContainerManager::rootless_podman() const {
    return tool() == ContainerTool::Podman && podman_info_rootless().value_or(true);
}

bool ContainerManager::expect_rootless() const {
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        if (podman_rootless_) return *podman_rootless_;
    }
    return host_.env("CONTAINER_HOST").empty() && !host_.is_root();
}

std::vector<SetupFailure> ContainerManager::check_host() const {
    std::vector<SetupFailure> failures;
    const bool service = host_.account() == kServiceAccount;
    const std::string member = service ? kServiceAccount : "$USER";
    const std::string apply = service ? "sudo systemctl restart lemond" : kRelogin;
    const std::optional<ContainerTool> chosen = tool();
    if (chosen == ContainerTool::Docker) {
        const CommandResult info = host_.run(
            {"docker", "info", "--format", "{{.OperatingSystem}}|{{json .SecurityOptions}}"},
            kDockerInfoTimeoutSeconds, /*capture_stderr=*/false);
        const std::string install = podman_install_command(host_.read_file("/etc/os-release"));
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
                            {podman_install_command(host_.read_file("/etc/os-release"))}, ""});
    }
    if (expect_rootless() && !host_.gpu_devices_accessible()) {
        failures.push_back({"device_groups", "Account cannot open the GPU devices",
                            {"sudo usermod -aG video,render " + member}, apply});
    }
    return failures;
}

std::optional<SetupFailure> ContainerManager::check_podman() const {
    if (tool() != ContainerTool::Podman) return std::nullopt;
    // Only local rootless Podman maps container IDs onto lemonade's own ranges;
    // a root or remote Podman behind CONTAINER_HOST uses its own.
    if (host_.account() == kServiceAccount && expect_rootless() && host_.on_path("getsubids")) {
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
        return SetupFailure{"podman_info", "`podman info` fails",
                            {"Run `podman info` and fix the error it prints"}, ""};
    }
    // Rootful lemond connects at the container's address, and Podman 4's CNI
    // backend gives an internal network no gateway on the host to reach it by.
    // Rootless Podman publishes a port instead, so it works on CNI too.
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
        if (rootless) {
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
    if (rootless) {
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
    // systemd user manager, which lemond.service's account does not have, and
    // lemond reaches its one container through a published port. Rootful
    // Podman keeps DNS: netavark gives an internal network without it no
    // gateway, and lemond connects at the container's address.
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

ContainerMount ContainerManager::model_mount(const std::string& host_path,
                                            const std::string& destination) {
    std::error_code ec;
    const fs::path source = fs::canonical(fs::path(host_path), ec);
    if (ec) {
        throw std::runtime_error("Model file '" + host_path + "' does not exist");
    }
    return {source.string(), destination};
}

}  // namespace utils
}  // namespace lemon
