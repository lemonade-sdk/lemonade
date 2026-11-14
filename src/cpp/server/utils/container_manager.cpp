#include "lemon/utils/container_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

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
    if (first == std::string::npos)
        return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        line = trim(line);
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

const char* tool_name(ContainerTool tool) {
    return tool == ContainerTool::Podman ? "podman" : "docker";
}

#ifndef _WIN32
std::optional<gid_t> lookup_group(const std::string& group) {
    struct group entry = {};
    struct group* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getgrnam_r(group.c_str(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) {
        return std::nullopt;
    }
    return found->gr_gid;
}
bool account_opens_gpu_devices() {
    std::vector<fs::path> nodes = {"/dev/kfd"};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
        if (entry.path().filename().string().rfind("renderD", 0) == 0) {
            nodes.push_back(entry.path());
        }
    }
    for (const auto& node : nodes) {
        if (fs::exists(node, ec) && ::access(node.c_str(), R_OK | W_OK) != 0)
            return false;
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
    if (argv.empty())
        return result;
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

}

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
    host.stream = [](const auto& argv, int timeout, const auto& on_line) {
        return ProcessManager::run_process_with_output(argv.front(), {argv.begin() + 1, argv.end()},
                                                       on_line, "", timeout, true);
    };
    host.canonical = [](const std::string& path) { return fs::canonical(path).string(); };
    host.file_kind = [](const std::string& path) {
        if (fs::is_regular_file(path))
            return ContainerFileKind::File;
        if (fs::is_directory(path))
            return ContainerFileKind::Directory;
        return ContainerFileKind::Other;
    };
    host.files = [](const std::string& path) {
        std::vector<std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_directory() && !entry.is_symlink())
                continue;
            if (!entry.is_regular_file())
                throw std::runtime_error("Unsupported model entry: " + entry.path().string());
            files.push_back(entry.path().string());
        }
        return files;
    };
    host.sleep = [](auto duration) { std::this_thread::sleep_for(duration); };
    return host;
}

ContainerManager::ContainerManager(ContainerHost host) : host_(std::move(host)) {
    if (!host_.on_path || !host_.gpu_devices_accessible || !host_.group_id || !host_.account ||
        !host_.env || !host_.is_root || !host_.read_file || !host_.run || !host_.stream ||
        !host_.canonical || !host_.file_kind || !host_.files || !host_.sleep) {
        throw std::invalid_argument("Incomplete container host effects");
    }
}

std::optional<ContainerTool> ContainerManager::tool() const {
    if (host_.on_path("podman"))
        return ContainerTool::Podman;
    if (host_.on_path("docker"))
        return ContainerTool::Docker;
    return std::nullopt;
}

std::optional<bool> ContainerManager::podman_info_rootless() const {
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        if (podman_rootless_)
            return podman_rootless_;
    }
    const CommandResult info = host_.run(
        {"podman", "info", "--format", "{{.Host.Security.Rootless}} {{.Host.NetworkBackend}}"},
        kCommandTimeoutSeconds, false);
    if (info.exit_code != 0)
        return std::nullopt;
    std::istringstream fields(info.output);
    std::string rootless_field;
    std::string backend;
    fields >> rootless_field >> backend;
    std::string extra;
    if ((rootless_field != "true" && rootless_field != "false") ||
        (backend != "netavark" && backend != "cni") || (fields >> extra))
        return std::nullopt;
    const bool rootless = rootless_field == "true";
    std::lock_guard<std::mutex> lock(podman_mode_mutex_);
    podman_rootless_ = rootless;
    podman_network_backend_ = backend;
    return rootless;
}

bool ContainerManager::rootless_podman() const {
    if (tool() != ContainerTool::Podman)
        return false;
    const auto rootless = podman_info_rootless();
    if (!rootless)
        throw std::runtime_error("Cannot determine Podman runtime mode");
    return *rootless;
}

bool ContainerManager::expect_rootless() const {
    {
        std::lock_guard<std::mutex> lock(podman_mode_mutex_);
        if (podman_rootless_)
            return *podman_rootless_;
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
            kDockerInfoTimeoutSeconds, false);
        const std::string install = podman_install_command(host_.read_file("/etc/os-release"));
        if (info.exit_code != 0) {
            failures.push_back({"docker_access",
                                "The Docker daemon refuses the account",
                                {"sudo usermod -aG docker " + member},
                                apply});
        } else if (info.output.find("Docker Desktop") != std::string::npos) {
            failures.push_back(
                {"docker_desktop", "Docker Desktop cannot give containers the GPU", {install}, ""});
        } else if (info.output.find("name=rootless") != std::string::npos) {
            failures.push_back({"docker_rootless",
                                "Rootless Docker cannot run container backends",
                                {install},
                                ""});
        }
        return failures;
    }
    if (!chosen) {
        failures.push_back({"container_tool",
                            "podman is not on PATH",
                            {podman_install_command(host_.read_file("/etc/os-release"))},
                            ""});
    }
    if (expect_rootless() && !host_.gpu_devices_accessible()) {
        failures.push_back({"device_groups",
                            "Account cannot open the GPU devices",
                            {"sudo usermod -aG render " + member},
                            apply});
    }
    return failures;
}

std::optional<SetupFailure> ContainerManager::check_podman() const {
    if (tool() != ContainerTool::Podman)
        return std::nullopt;
    if (host_.account() == kServiceAccount && expect_rootless() && host_.on_path("getsubids")) {
        const auto has_range = [this](const std::vector<std::string>& argv) {
            const CommandResult result = host_.run(argv, kCommandTimeoutSeconds, false);
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
        return SetupFailure{"podman_info",
                            "`podman info` fails",
                            {"Run `podman info` and fix the error it prints"},
                            ""};
    }
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
            if (service)
                apply = "sudo systemctl restart lemond";
        }
        return SetupFailure{
            "rootful_cni",
            "Rootful Podman's CNI network backend gives lemond no route to its containers",
            {step},
            apply};
    }
    return std::nullopt;
}

std::optional<SetupFailure> ContainerManager::check_selinux() const {
    const bool enforcing = host_.read_file("/sys/fs/selinux/enforce").compare(0, 1, "1") == 0;
    const bool devices_allowed =
        host_.read_file("/sys/fs/selinux/booleans/container_use_devices").compare(0, 1, "0") != 0;
    if (enforcing && !devices_allowed) {
        return SetupFailure{"selinux",
                            "SELinux is enforcing and the container_use_devices boolean is off",
                            {"sudo setsebool -P container_use_devices 1"},
                            ""};
    }
    return std::nullopt;
}

std::vector<std::string> SetupReport::ids() const {
    std::vector<std::string> ids;
    for (const auto& failure : failures)
        ids.push_back(failure.id);
    return ids;
}

std::string SetupReport::message() const {
    std::string text;
    for (const auto& failure : failures) {
        if (!text.empty())
            text += '\n';
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
    if (steps.size() == 1)
        return steps.front();
    std::string text;
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i > 0)
            text += '\n';
        text += std::to_string(i + 1) + ". " + steps[i];
    }
    return text;
}

std::string SetupReport::text() const {
    std::string text;
    for (const auto& line : lines_of(message()))
        text += "\n  - " + line;
    text += "\nTo fix it:";
    for (const auto& line : lines_of(action()))
        text += "\n  " + line;
    return text;
}

std::vector<std::string> ContainerManager::run_command(const ContainerRunSpec& spec) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    require_image(spec.image);
    if (spec.name != container_name(spec.recipe, spec.backend, spec.model) ||
        spec.network != spec.name || spec.port < 1 || spec.port > 65535)
        throw std::invalid_argument("Invalid owned container run spec");
    SetupReport report{check_host()};
    if (auto failure = check_podman())
        report.failures.push_back(*failure);
    if (std::find(spec.devices.begin(), spec.devices.end(), "/dev/kfd") != spec.devices.end()) {
        if (auto failure = check_selinux())
            report.failures.push_back(*failure);
    }
    if (!report.empty())
        throw std::runtime_error(report.text());
    const bool rootless = rootless_podman();

    std::vector<std::string> argv = {
        tool_name(*chosen),
        "run",
        "--rm",
        "--init",
        "--name",
        spec.name,
        "--label",
        kLabel,
        "--label",
        std::string(kLabel) + ".recipe=" + spec.recipe,
        "--label",
        std::string(kLabel) + ".backend=" + spec.backend,
        "--label",
        std::string(kLabel) + ".model=" + spec.model,
        "--label",
        std::string(kLabel) + ".port=" + std::to_string(spec.port),
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
        const std::string gid = host_.group_id("render");
        if (gid.empty()) throw std::runtime_error("The render group is required for container GPU access");
        argv.push_back("--group-add");
        argv.push_back(gid);
    }
    for (const auto& capability : spec.cap_add) {
        if (!std::regex_match(capability, std::regex("[A-Z][A-Z0-9_]*")) || capability == "ALL") {
            throw std::invalid_argument("Invalid granted container capability");
        }
        argv.push_back("--cap-add");
        argv.push_back(capability);
    }
    for (const auto& mount : spec.mounts) {
        if (mount.source.find_first_of(":\r\n") != std::string::npos ||
            mount.source.find('\0') != std::string::npos || !fs::path(mount.source).is_absolute()) {
            throw std::runtime_error("Cannot mount " + mount.source +
                                     " into a container: invalid source path");
        }
        const fs::path destination(mount.destination);
        if (!destination.is_absolute() || destination.lexically_normal() != destination ||
            mount.destination.rfind("/mnt/models/", 0) != 0 ||
            mount.destination.find_first_of(":\r\n") != std::string::npos ||
            mount.destination.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid container mount destination");
        }
        argv.push_back("-v");
        argv.push_back(mount.source + ":" + mount.destination + ":ro,z");
    }
    argv.push_back("--env");
    argv.push_back("HOME=/var/tmp");
    argv.push_back("--env");
    argv.push_back("TMPDIR=/var/tmp");
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

CommandResult ContainerManager::invoke(const std::vector<std::string>& args, int timeout_seconds,
                                       bool capture_stderr) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    std::vector<std::string> argv = {tool_name(*chosen)};
    argv.insert(argv.end(), args.begin(), args.end());
    return host_.run(argv, timeout_seconds, capture_stderr);
}

bool ContainerManager::has_image(const std::string& image) const {
    require_image(image);
    if (!tool())
        return false;
    return invoke({"image", "inspect", "--format", "{{.Id}}", image}, kCommandTimeoutSeconds)
               .exit_code == 0;
}

void ContainerManager::pull(const std::string& image,
                            const DownloadProgressCallback& progress) const {
    const std::optional<ContainerTool> chosen = tool();
    if (!chosen) {
        throw std::runtime_error("Neither podman nor docker is on PATH");
    }
    require_image(image);
    LOG(INFO, "Container") << "Pulling " << image << std::endl;

    DownloadProgress p;
    p.file = image;
    p.file_index = 1;
    p.total_files = 1;
    if (progress && !progress(p)) {
        throw std::runtime_error("Pull of " + image + " cancelled");
    }
    std::set<std::string> layers;
    std::set<std::string> finished;
    std::string output;
    bool cancelled = false;
    const int exit_code = host_.stream(
        {tool_name(*chosen), "pull", image}, kPullTimeoutSeconds, [&](const std::string& raw) {
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
            if (layer.empty())
                return true;
            layers.insert(layer);
            if (done)
                finished.insert(layer);
            if (progress) {
                p.percent = static_cast<int>(finished.size() * 99 / layers.size());
                if (!progress(p)) {
                    cancelled = true;
                    return false;
                }
            }
            return true;
        });

    if (cancelled) {
        throw std::runtime_error("Pull of " + image + " cancelled");
    }
    if (exit_code != 0) {
        throw std::runtime_error("Failed to pull " + image + ": " + trim(output));
    }
    p.percent = 100;
    p.complete = true;
    if (progress)
        progress(p);
}

void ContainerManager::remove_image(const std::string& image) const {
    require_image(image);
    if (!tool())
        return;
    const CommandResult result = invoke({"rmi", image}, kCommandTimeoutSeconds);
    if (result.exit_code != 0) {
        LOG(WARNING, "Container") << "Could not remove image " << image << ": "
                                  << trim(result.output) << std::endl;
    }
}

void ContainerManager::create_network(const std::string& name) const {
    remove_network(name);
    std::vector<std::string> args = {"network", "create", "--internal", "--label", kLabel};
    if (rootless_podman())
        args.push_back("--disable-dns");
    args.push_back(name);
    const CommandResult result = invoke(args, kCommandTimeoutSeconds);
    if (result.exit_code != 0) {
        throw std::runtime_error("Could not create the network " + name + ": " +
                                 trim(result.output));
    }
}

void ContainerManager::remove_container(const std::string& name) const {
    if (!tool())
        return;
    const auto result = invoke({"rm", "-f", name}, kCommandTimeoutSeconds);
    if (result.exit_code != 0) {
        LOG(DEBUG, "Container") << "rm " << name << " failed: " << trim(result.output) << std::endl;
    }
}

void ContainerManager::remove_network(const std::string& name) const {
    if (!tool())
        return;
    // The runtime detaches --rm containers from their network asynchronously.
    for (int attempt = 0; attempt < 20; ++attempt) {
        const CommandResult result = invoke({"network", "rm", name}, kCommandTimeoutSeconds);
        if (result.exit_code == 0)
            return;
        std::string output = result.output;
        std::transform(output.begin(), output.end(), output.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (output.find("no such network") != std::string::npos ||
            output.find("not found") != std::string::npos) {
            return;
        }
        host_.sleep(std::chrono::milliseconds(250));
    }
    LOG(WARNING, "Container") << "Could not remove network " << name << std::endl;
}

void ContainerManager::stop(const std::string& name) const {
    if (!tool())
        return;
    const auto attempt = [&](const auto& action) {
        try {
            action();
        } catch (const std::exception& error) {
            LOG(WARNING, "Container")
                << "Cleanup of " << name << " failed: " << error.what() << std::endl;
        }
    };
    attempt([&] {
        const auto result =
            invoke({"stop", "--time", std::to_string(kStopSeconds), name}, kStopSeconds + 30);
        if (result.exit_code != 0) {
            LOG(WARNING, "Container")
                << "stop " << name << " failed: " << trim(result.output) << std::endl;
        }
    });
    attempt([&] { remove_container(name); });
    attempt([&] { remove_network(name); });
}

std::string ContainerManager::address(const std::string& name, const std::string& network,
                                      int timeout_seconds) const {
    if (name.empty() || network != name || timeout_seconds < 1 || timeout_seconds > 5) {
        throw std::invalid_argument("Invalid owned container address request");
    }
    const auto result = invoke({"inspect", "--type", "container", name}, timeout_seconds, false);
    if (result.exit_code != 0)
        throw ContainerAddressPending("Cannot inspect owned container " + name);
    using json = nlohmann::json;
    std::vector<std::set<std::string>> keys;
    const auto value =
        json::parse(result.output, [&keys](int, json::parse_event_t event, json& parsed) {
            if (event == json::parse_event_t::object_start)
                keys.emplace_back();
            if (event == json::parse_event_t::key &&
                !keys.back().insert(parsed.get<std::string>()).second) {
                throw std::runtime_error("Ambiguous container inspection field");
            }
            if (event == json::parse_event_t::object_end)
                keys.pop_back();
            return true;
        });
    if (!value.is_array() || value.size() != 1)
        throw std::runtime_error("Ambiguous container inspection");
    const auto& container = value.at(0);
    const auto actual_name = container.at("Name").get<std::string>();
    if ((actual_name != name && actual_name != "/" + name) ||
        container.at("Config").at("Labels").value(kLabel, std::string("unowned")) != "") {
        throw std::runtime_error("Inspected container is not owned");
    }
    const auto& networks = container.at("NetworkSettings").at("Networks");
    if (!networks.is_object() || !networks.contains(network))
        throw ContainerAddressPending("Owned network is missing");
    const auto& attachment = networks.at(network);
    const auto address = attachment.at("IPAddress").get<std::string>();
    if (!attachment.value("GlobalIPv6Address", std::string()).empty()) {
        throw std::runtime_error("Ambiguous container address");
    }
    if (address.empty())
        throw ContainerAddressPending("Owned container address is missing");
    // Only an unambiguous unicast IPv4 literal may become an HTTP host.
    std::istringstream octets(address);
    int parts[4] = {};
    for (int i = 0; i < 4; ++i) {
        std::string part;
        if (!std::getline(octets, part, '.') || part.empty() || part.size() > 3 ||
            (part.size() > 1 && part.front() == '0') ||
            !std::all_of(part.begin(), part.end(),
                         [](unsigned char c) { return c >= '0' && c <= '9'; })) {
            throw std::runtime_error("Invalid container address");
        }
        parts[i] = std::stoi(part);
        if (parts[i] > 255)
            throw std::runtime_error("Invalid container address");
    }
    if (octets.rdbuf()->in_avail() || address.back() == '.' || parts[0] == 0 || parts[0] == 127 ||
        parts[0] >= 224 || (parts[0] == 169 && parts[1] == 254)) {
        throw std::runtime_error("Invalid container address");
    }
    return address;
}

std::string ContainerManager::container_name(const std::string& recipe, const std::string& backend,
                                             const std::string& model) {
    return "lemonade-" + name_token(recipe) + "-" + name_token(backend) + "-" + name_token(model);
}

std::string ContainerManager::podman_install_command(const std::string& os_release) {
    std::string id;
    std::string id_like;
    for (const auto& line : lines_of(os_release)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "ID")
            id = value;
        if (key == "ID_LIKE")
            id_like = value;
    }

    std::vector<std::string> candidates;
    if (!id.empty())
        candidates.push_back(id);
    std::istringstream words(id_like);
    for (std::string word; words >> word;)
        candidates.push_back(word);

    for (const auto& candidate : candidates) {
        if (candidate == "debian")
            return "sudo apt install podman";
        if (candidate == "fedora")
            return "sudo dnf install podman";
        if (candidate == "arch")
            return "sudo pacman -S podman";
    }
    return "Install Podman with the host's package manager";
}

bool ContainerManager::allowed_image(const std::string& image) {
    static const std::regex repository("(docker\\.io/kyuz0/|ghcr\\.io/peonist-ai/"
                                       ")[a-z0-9]+([._-][a-z0-9]+)*(/[a-z0-9]+([._-][a-z0-9]+)*)*");
    return std::regex_match(image, repository);
}

std::optional<ContainerPin> parse_container_pin(const std::string& pin) {
    static const std::regex syntax("([A-Za-z0-9_][A-Za-z0-9_.-]{0,127})@(sha256:[0-9a-f]{64})");
    std::smatch match;
    if (!std::regex_match(pin, match, syntax))
        return std::nullopt;
    return ContainerPin{match[1], match[2]};
}

std::string short_container_pin(const std::string& pin) {
    const auto parsed = parse_container_pin(pin);
    return parsed ? parsed->tag + "@" + parsed->digest.substr(7, 12) : pin;
}

std::string container_image(const std::string& repository, const std::string& pin) {
    const auto parsed = parse_container_pin(pin);
    if (!ContainerManager::allowed_image(repository) || !parsed) {
        throw std::invalid_argument("Expected an allowed repository and <tag>@sha256:<digest> pin");
    }
    return repository + "@" + parsed->digest;
}

void ContainerManager::require_image(const std::string& image) {
    const auto at = image.find('@');
    if (at == std::string::npos || !allowed_image(image.substr(0, at)) ||
        !parse_container_pin("pin@" + image.substr(at + 1))) {
        throw std::invalid_argument("Expected an allowed immutable container image");
    }
}

std::vector<ContainerMount> ContainerManager::model_mounts(const std::string& host_path,
                                                           const std::string& destination) const {
    const fs::path target(destination);
    if (!target.is_absolute() || target.lexically_normal() != target ||
        destination.rfind("/mnt/models/", 0) != 0 ||
        destination.find_first_of(":\r\n") != std::string::npos ||
        destination.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid model mount target");
    const std::string source = host_.canonical(host_path);
    const auto kind = host_.file_kind(source);
    if (kind == ContainerFileKind::File)
        return {{source, destination}};
    if (kind != ContainerFileKind::Directory)
        throw std::runtime_error("Unsupported model file " + host_path);
    std::vector<ContainerMount> mounts;
    const auto named = fs::path(host_path).lexically_normal();
    std::set<std::string> destinations;
    // HF snapshot links resolve outside the directory, so mount their resolved files individually.
    for (const auto& file : host_.files(host_path)) {
        const auto relative = fs::path(file).lexically_relative(named);
        if (relative.empty() || relative.is_absolute())
            throw std::runtime_error("Invalid model inventory path");
        for (const auto& part : relative) {
            if (part == ".." || part == ".")
                throw std::runtime_error("Escaped model inventory path");
        }
        const auto resolved = host_.canonical(file);
        if (host_.file_kind(resolved) != ContainerFileKind::File)
            throw std::runtime_error("Unsupported model entry " + file);
        const auto inside = (target / relative).generic_string();
        if (!destinations.insert(inside).second)
            throw std::runtime_error("Ambiguous model mount target");
        mounts.push_back({resolved, inside});
    }
    std::sort(mounts.begin(), mounts.end(),
              [](const auto& a, const auto& b) { return a.destination < b.destination; });
    return mounts;
}

}
}
