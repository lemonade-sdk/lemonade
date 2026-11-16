// Asserts the container backends spec's Command Contract and setup checks
// against ContainerManager, with a fake host in place of Podman and Docker, and
// checks every container backend's descriptor and pin.

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/gufo/gufo_server.h"
#include "lemon/backends/halogen/halogen_server.h"
#include "lemon/utils/container_manager.h"
#include "lemon/utils/network_utils.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using lemon::utils::ContainerHost;
using lemon::utils::ContainerManager;
using lemon::utils::ContainerRunSpec;

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string join(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
        if (!out.empty()) out += ' ';
        out += arg;
    }
    return out;
}

struct FakeHost {
    std::set<std::string> tools;
    bool gpu_access = false;
    std::string account = "alice";
    std::map<std::string, std::string> subids;  // getsubids arguments -> output
    // `docker info`'s exit code and its OperatingSystem|SecurityOptions answer.
    int docker_info_exit = 0;
    std::string docker_info = "Ubuntu 24.04 LTS|[\"name=apparmor,profile=default\"]";
    // `podman info`'s exit code and its Host.Security.Rootless answer.
    int podman_info_exit = 0;
    std::string podman_rootless = "true";
    std::string network_backend = "netavark";
    std::string oci_runtime = "crun";
    // What Podman prints on stderr, which reaches `output` only when captured.
    std::string stderr_noise =
        "time=\"2026-10-17T00:57:31Z\" level=warning msg=\"Falling back to --cgroup-manager=cgroupfs\"\n";
    std::map<std::string, std::string> files = {{"/etc/os-release", "ID=ubuntu\nID_LIKE=debian\n"}};
    std::map<std::string, std::string> env;
    std::set<std::string> paths;  // what exists() finds
    std::string gpu_group;        // owner of the GPU device nodes
    // `inspect` answers for these names, as {{.Id}}|{{json .Mounts}}.
    std::map<std::string, std::string> containers;
    // `network inspect`'s .Internal answer by network name; unlisted ones are false.
    std::map<std::string, std::string> internal_networks;
    bool root = false;
    // How many commands the manager has run on the host.
    mutable int commands = 0;
    mutable std::vector<std::vector<std::string>> runs;

    ContainerHost host() const {
        ContainerHost h;
        h.on_path = [this](const std::string& name) { return tools.count(name) > 0; };
        h.exists = [this](const std::string& path) { return paths.count(path) > 0; };
        h.gpu_devices_accessible = [this]() { return gpu_access; };
        h.gpu_device_group = [this]() { return gpu_group; };
        h.account = [this]() { return account; };
        h.group_id = [](const std::string& group) {
            return group == "render" ? std::string("990") : std::string();
        };
        h.env = [this](const std::string& name) {
            const auto it = env.find(name);
            return it == env.end() ? std::string() : it->second;
        };
        h.is_root = [this]() { return root; };
        h.read_file = [this](const std::string& path) {
            const auto it = files.find(path);
            return it == files.end() ? std::string() : it->second;
        };
        h.run = [this](const std::vector<std::string>& argv, int, bool capture_stderr) {
            ++commands;
            runs.push_back(argv);
            lemon::utils::CommandResult result;
            if (argv.size() > 1 && argv[0] == "podman" && argv[1] == "info") {
                result.exit_code = podman_info_exit;
                result.output = podman_rootless + " " + network_backend + " " + oci_runtime + "\n";
            } else if (argv.size() > 2 && argv[1] == "network" && argv[2] == "inspect") {
                result.exit_code = 0;
                for (size_t i = 5; i < argv.size(); ++i) {
                    const auto it = internal_networks.find(argv[i]);
                    result.output += argv[i] + " " + (it == internal_networks.end() ? "false" : it->second) + "\n";
                }
            } else if (argv.size() > 1 && argv[1] == "network") {
                result.exit_code = 0;
            } else if (argv.size() > 1 && argv[1] == "inspect") {
                const auto it = containers.find(argv.back());
                result.exit_code = it == containers.end() ? 1 : 0;
                if (it != containers.end()) result.output = it->second + "\n";
            } else if (argv.front() == "getsubids") {
                const auto it = subids.find(join({argv.begin() + 1, argv.end()}));
                result.exit_code = it == subids.end() ? 1 : 0;
                if (it != subids.end()) result.output = it->second;
            }
            if (argv.size() > 1 && argv[0] == "docker" && argv[1] == "info") {
                result = {docker_info_exit, docker_info + "\n"};
            }
            if (capture_stderr) result.output += stderr_noise;
            return result;
        };
        return h;
    }
};

// The spec's Complete Example: Qwen3-4B-GGUF on llamacpp:nathanw.
ContainerRunSpec example_spec() {
    ContainerRunSpec spec;
    spec.name = "lemonade-13305-llamacpp-nathanw-Qwen3-4B-GGUF";
    spec.recipe = "llamacpp";
    spec.backend = "nathanw";
    spec.model = "Qwen3-4B-GGUF";
    spec.port = 8001;
    spec.network = spec.name;
    spec.devices = {"/dev/dri"};
    spec.mounts = {{"/home/alice/model.gguf", "/mnt/models/Qwen3-4B-Q4_K_M.gguf"}};
    spec.image = "docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:abc";
    spec.command = {"llama-server", "-m", "/mnt/models/Qwen3-4B-Q4_K_M.gguf", "--ctx-size",
                    "8192", "--port", "8001", "--host", "0.0.0.0"};
    return spec;
}

void test_run_command() {
    const std::string common =
        " run --rm --init --name lemonade-13305-llamacpp-nathanw-Qwen3-4B-GGUF"
        " --label ai.lemonade"
        " --label ai.lemonade.recipe=llamacpp"
        " --label ai.lemonade.backend=nathanw"
        " --label ai.lemonade.model=Qwen3-4B-GGUF"
        " --label ai.lemonade.port=8001"
        " --cap-drop=all --security-opt=no-new-privileges"
        " --pull=never --network=lemonade-13305-llamacpp-nathanw-Qwen3-4B-GGUF"
        " --device /dev/dri";
    const std::string mount_and_home =
        " -v /home/alice/model.gguf:/mnt/models/Qwen3-4B-Q4_K_M.gguf:ro,z"
        " --env HOME=/tmp";
    const std::string image_and_program =
        " docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:abc"
        " llama-server -m /mnt/models/Qwen3-4B-Q4_K_M.gguf --ctx-size 8192 --port 8001"
        " --host 0.0.0.0";

    FakeHost podman;
    podman.tools = {"podman", "docker"};
    const std::string podman_command = join(ContainerManager(podman.host()).run_command(example_spec()));
    check("podman run matches the spec's Complete Example",
          podman_command == "podman" + common + " --group-add keep-groups" + mount_and_home +
                                " -p 127.0.0.1:8001:8001" + image_and_program);

    FakeHost user_socket;
    user_socket.tools = {"podman"};
    user_socket.env = {{"CONTAINER_HOST", "unix:///run/user/1000/podman/podman.sock"},
                       {"XDG_RUNTIME_DIR", "/run/user/1000"}};
    const std::string user_socket_command =
        join(ContainerManager(user_socket.host()).run_command(example_spec()));
    check("podman run through the account's podman.socket keeps groups by annotation",
          user_socket_command == "podman" + common +
                                     " --annotation run.oci.keep_original_groups=1" +
                                     mount_and_home + " -p 127.0.0.1:8001:8001" +
                                     image_and_program);

    FakeHost docker;
    docker.tools = {"docker"};
    const std::string docker_command = join(ContainerManager(docker.host()).run_command(example_spec()));
    check("docker run adds the host's render ID and publishes no port",
          docker_command == "docker" + common + " --group-add 990" +
                                mount_and_home + image_and_program);

    FakeHost rootful;
    rootful.tools = {"podman"};
    rootful.podman_rootless = "false";
    const ContainerManager rootful_manager(rootful.host());
    check("rootful podman run takes Docker's group IDs and publishes no port",
          !rootful_manager.rootless_podman() &&
              join(rootful_manager.run_command(example_spec())) ==
                  "podman" + common + " --group-add 990" + mount_and_home +
                      image_and_program);

    ContainerRunSpec policy = example_spec();
    policy.devices = {"/dev/dri", "/dev/kfd"};
    policy.cap_add = {"SYS_PTRACE"};
    policy.env = {{"HIP_VISIBLE_DEVICES", "0"}, {"ROCBLAS_USE_HIPBLASLT", "1"}};
    const std::string policy_command = join(ContainerManager(podman.host()).run_command(policy));
    check("a policy's permissions and the command's variables follow the devices",
          policy_command.find(" --device /dev/dri --device /dev/kfd --group-add keep-groups"
                              " --cap-add SYS_PTRACE -v ") != std::string::npos &&
              policy_command.find(" --env HOME=/tmp --env HIP_VISIBLE_DEVICES=0"
                                  " --env ROCBLAS_USE_HIPBLASLT=1 -p ") != std::string::npos);

    ContainerRunSpec colon = example_spec();
    colon.mounts = {{"/home/alice/a:b.gguf", "/mnt/models/a:b.gguf"}};
    bool refused = false;
    try {
        ContainerManager(podman.host()).run_command(colon);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    check("a model path containing ':' is refused, since -v would split it", refused);
}

void test_tool_choice_and_setup() {
    FakeHost none;
    const ContainerManager no_tool(none.host());
    check("no tool is chosen when neither is on PATH", !no_tool.tool());
    const lemon::utils::SetupReport fresh{no_tool.check_host()};
    check("with no tool, every host step is reported at once: install Podman, then its groups",
          fresh.ids() == std::vector<std::string>({"container_tool", "device_groups"}));
    check("the host checks run no command", none.commands == 0);
    check("the steps are numbered, logging out last",
          fresh.action() ==
              "1. sudo apt install podman\n2. sudo usermod -aG render $USER\n"
              "3. Log out and back in");
    check("error text lists the problems, then the steps",
          fresh.text() ==
              "\n  - podman is not on PATH\n  - Account cannot open the GPU devices\n"
              "To fix it:\n  1. sudo apt install podman\n"
              "  2. sudo usermod -aG render $USER\n  3. Log out and back in");

    FakeHost both;
    both.tools = {"podman", "docker"};
    const lemon::utils::SetupReport groups{ContainerManager(both.host()).check_host()};
    check("Podman needs the account to open the GPU devices",
          groups.ids() == std::vector<std::string>({"device_groups"}) &&
              groups.message() == "Account cannot open the GPU devices" &&
              groups.action() == "1. sudo usermod -aG render $USER\n"
                                 "2. Log out and back in");
    both.gpu_access = true;
    check("Podman passes once the account can open the GPU devices", ContainerManager(both.host()).check_host().empty());

    FakeHost remote;
    remote.tools = {"podman"};
    remote.env["CONTAINER_HOST"] = "unix:///run/podman/podman.sock";
    check("before podman info runs, Podman behind CONTAINER_HOST needs neither group",
          ContainerManager(remote.host()).check_host().empty() && remote.commands == 0);
    FakeHost own_socket;
    own_socket.tools = {"podman"};
    own_socket.env = {{"CONTAINER_HOST", "unix:///run/user/1000/podman/podman.sock"},
                      {"XDG_RUNTIME_DIR", "/run/user/1000"}};
    check("before podman info runs, the account's own podman.socket needs the GPU devices",
          lemon::utils::SetupReport{ContainerManager(own_socket.host()).check_host()}.ids() ==
                  std::vector<std::string>({"device_groups"}) &&
              own_socket.commands == 0);
    FakeHost as_root;
    as_root.tools = {"podman"};
    as_root.root = true;
    check("before podman info runs, Podman run as root needs neither group",
          ContainerManager(as_root.host()).check_host().empty());

    FakeHost rootful;
    rootful.tools = {"podman"};
    rootful.podman_rootless = "false";
    const ContainerManager rootful_manager(rootful.host());
    check("rootful Podman passes podman info", !rootful_manager.check_podman());
    check("once podman info says rootful, Podman needs neither group",
          rootful_manager.check_host().empty());

    FakeHost noisy;
    noisy.tools = {"podman"};
    check("Podman's stderr warnings do not hide that it is rootless",
          ContainerManager(noisy.host()).rootless_podman());

    FakeHost broken;
    broken.tools = {"podman"};
    broken.gpu_access = true;
    broken.podman_info_exit = 125;
    const auto silent = ContainerManager(broken.host()).check_podman();
    check("Podman must answer podman info",
          silent && silent->id == "podman_info" && silent->message == "`podman info` fails" &&
              lemon::utils::SetupReport{{*silent}}.action() ==
                  "Run `podman info` and fix the error it prints");

    FakeHost no_socket = broken;
    no_socket.env = {{"CONTAINER_HOST", "unix:///run/user/1000/podman/podman.sock"},
                     {"XDG_RUNTIME_DIR", "/run/user/1000"}};
    const auto unreachable = ContainerManager(no_socket.host()).check_podman();
    check("an unreachable podman.socket of the account's own asks to start it",
          unreachable && unreachable->id == "podman_info" &&
              lemon::utils::SetupReport{{*unreachable}}.action() ==
                  "systemctl --user enable --now podman.socket");

    FakeHost elsewhere = broken;
    elsewhere.env = {{"CONTAINER_HOST", "unix:///run/podman-lemonade.sock"}};
    const auto remote_info = ContainerManager(elsewhere.host()).check_podman();
    check("a failing Podman behind another CONTAINER_HOST names it",
          remote_info && remote_info->id == "podman_info" &&
              lemon::utils::SetupReport{{*remote_info}}.action() ==
                  "Run `podman info` with CONTAINER_HOST=unix:///run/podman-lemonade.sock and fix "
                  "the error it prints");

    FakeHost docker;
    docker.tools = {"docker"};
    docker.docker_info_exit = 1;
    const lemon::utils::SetupReport refused{ContainerManager(docker.host()).check_host()};
    check("Docker needs the daemon to accept the account",
          refused.ids() == std::vector<std::string>({"docker_access"}) &&
              refused.message() == "The Docker daemon refuses the account" &&
              refused.action() == "1. sudo usermod -aG docker $USER\n2. Log out and back in");
    docker.docker_info_exit = 0;
    check("Docker passes once the daemon accepts the account",
          ContainerManager(docker.host()).check_host().empty());
    docker.docker_info = "Docker Desktop|[\"name=seccomp,profile=unconfined\"]";
    const lemon::utils::SetupReport desktop{ContainerManager(docker.host()).check_host()};
    check("Docker Desktop is refused, with Podman as the fix",
          desktop.ids() == std::vector<std::string>({"docker_desktop"}) &&
              desktop.action() == "sudo apt install podman");
    docker.docker_info = "Ubuntu 24.04 LTS|[\"name=seccomp,profile=builtin\",\"name=rootless\"]";
    const lemon::utils::SetupReport rootless{ContainerManager(docker.host()).check_host()};
    check("rootless Docker is refused, with Podman as the fix",
          rootless.ids() == std::vector<std::string>({"docker_rootless"}) &&
              rootless.action() == "sudo apt install podman");

    lemon::utils::SetupReport both_relogins{groups.failures};
    both_relogins.failures.push_back(refused.failures.front());
    check("logging out and back in is listed once, last",
          both_relogins.action() ==
              "1. sudo usermod -aG render $USER\n2. sudo usermod -aG docker $USER\n"
              "3. Log out and back in");
}

// lemond.service runs as the lemonade account, so each fix names it and
// restarts the service.
void test_system_service_setup() {
    FakeHost podman;
    podman.account = "lemonade";
    podman.tools = {"podman", "getsubids"};
    podman.subids = {{"lemonade", "0: lemonade 165536 65536\n"}};
    const lemon::utils::SetupReport groups{ContainerManager(podman.host()).check_host()};
    check("the service's group fix names lemonade and restarts lemond",
          groups.ids() == std::vector<std::string>({"device_groups"}) &&
              groups.action() == "1. sudo usermod -aG render lemonade\n"
                                 "2. sudo systemctl restart lemond");
    check("check_host runs no commands",
          podman.commands == 0);

    const auto subids = ContainerManager(podman.host()).check_podman();
    check("the service needs subordinate GIDs as well as UIDs",
          subids && subids->id == "subordinate_ids" &&
              lemon::utils::SetupReport{{*subids}}.action() ==
                  "1. Ask the host's administrator to allocate 65,536 subordinate UIDs and "
                  "GIDs to lemonade\n"
                  "2. sudo systemctl restart lemond");
    podman.subids["-g lemonade"] = "0: lemonade 165536 65536\n";
    podman.gpu_access = true;
    const ContainerManager ready(podman.host());
    check("the service passes with both ranges and GPU access",
          ready.check_host().empty() && !ready.check_podman());

    FakeHost user = podman;
    user.account = "alice";
    user.subids.clear();
    check("a user-started lemond skips the subordinate ID check",
          !ContainerManager(user.host()).check_podman());

    FakeHost docker;
    docker.account = "lemonade";
    docker.tools = {"docker"};
    docker.docker_info_exit = 1;
    const lemon::utils::SetupReport refused{ContainerManager(docker.host()).check_host()};
    check("the service's Docker fix names lemonade and restarts lemond",
          refused.action() == "1. sudo usermod -aG docker lemonade\n"
                              "2. sudo systemctl restart lemond");
}

// The network create command, as run for each tool.
std::string network_create(const FakeHost& fake) {
    const ContainerManager manager(fake.host());
    manager.create_network("lemonade-n");
    for (const auto& argv : fake.runs) {
        if (argv.size() > 2 && argv[1] == "network" && argv[2] == "create") return join(argv);
    }
    return "";
}

void test_create_network() {
    FakeHost podman;
    podman.tools = {"podman"};
    check("rootless Podman's per-model network is internal and has no DNS server",
          network_create(podman) ==
              "podman network create --internal --label ai.lemonade --disable-dns lemonade-n");
    FakeHost rootful;
    rootful.tools = {"podman"};
    rootful.podman_rootless = "false";
    check("rootful Podman's per-model network keeps its DNS server",
          network_create(rootful) == "podman network create --internal --label ai.lemonade lemonade-n");
    FakeHost docker;
    docker.tools = {"docker"};
    check("Docker's per-model network is internal",
          network_create(docker) == "docker network create --internal --label ai.lemonade lemonade-n");
}

// Rootful Podman through CONTAINER_HOST never uses lemonade's local subordinate
// IDs, so their absence must not block it.
void test_rootful_service() {
    FakeHost rootful_service;
    rootful_service.account = "lemonade";
    rootful_service.tools = {"podman", "getsubids"};
    rootful_service.env = {{"CONTAINER_HOST", "unix:///run/podman-lemonade.sock"}};
    rootful_service.podman_rootless = "false";
    const auto failure = ContainerManager(rootful_service.host()).check_podman();
    check("rootful Podman via CONTAINER_HOST needs no local subordinate IDs",
          !failure || failure->id != "subordinate_ids");
}

void test_rootful_cni() {
    // Rootful lemond connects to the container's address, which an internal
    // network on Podman 4's CNI backend leaves unreachable. Either the port is
    // published or setup reports the problem, instead of a load that times out.
    FakeHost cni;
    cni.tools = {"podman"};
    cni.podman_rootless = "false";
    cni.network_backend = "cni";
    const ContainerManager cni_manager(cni.host());
    const bool published =
        join(cni_manager.run_command(example_spec())).find(" -p 127.0.0.1:") != std::string::npos;
    check("rootful Podman on CNI publishes the port or reports a setup failure",
          published || cni_manager.check_podman().has_value());

    FakeHost cni_service = cni;
    cni_service.account = "lemonade";
    cni_service.env = {{"CONTAINER_HOST", "unix:///run/podman-lemonade.sock"}};
    const auto cni_failure = ContainerManager(cni_service.host()).check_podman();
    check("the service's CNI fix removes CONTAINER_HOST and restarts lemond",
          cni_failure && cni_failure->id == "rootful_cni" &&
              lemon::utils::SetupReport{{*cni_failure}}.action() ==
                  "1. Remove CONTAINER_HOST from /etc/default/lemond to use rootless Podman\n"
                  "2. sudo systemctl restart lemond");
    FakeHost netavark = cni;
    netavark.network_backend = "netavark";
    check("rootful Podman on netavark passes the setup check",
          !ContainerManager(netavark.host()).check_podman());
    FakeHost rootless_cni = cni;
    rootless_cni.podman_rootless = "true";
    check("rootless Podman on CNI passes the setup check, since it publishes a port",
          !ContainerManager(rootless_cni.host()).check_podman());
}

void test_install_commands() {
    const auto podman = [](const std::string& os_release) {
        return ContainerManager::install_command(os_release, "podman", "Podman");
    };
    check("Rocky Linux matches fedora through ID_LIKE",
          podman("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n") == "sudo dnf install podman");
    check("Fedora matches on ID", podman("ID=fedora\n") == "sudo dnf install podman");
    check("an unknown distribution gets the generic step",
          podman("ID=gentoo\n") == "Install Podman with the host's package manager");
    check("crun installs through the same package manager",
          ContainerManager::install_command("ID=ubuntu\nID_LIKE=debian\n", "crun", "crun") ==
                  "sudo apt install crun" &&
              ContainerManager::install_command("ID=gentoo\n", "crun", "crun") ==
                  "Install crun with the host's package manager");
}

void test_oci_runtime() {
    FakeHost runc;
    runc.tools = {"podman"};
    runc.gpu_access = true;
    runc.oci_runtime = "runc";
    const ContainerManager manager(runc.host());
    const auto failure = manager.check_podman();
    check("rootless Podman on runc asks for crun",
          failure && failure->id == "oci_runtime" &&
              failure->message ==
                  "Rootless Podman runs containers with runc, which cannot give them the GPU" &&
              lemon::utils::SetupReport{{*failure}}.action() == "sudo apt install crun");
    runc.oci_runtime = "crun";
    check("installing crun clears the failure without restarting lemond", !manager.check_podman());

    FakeHost fedora = runc;
    fedora.oci_runtime = "runc";
    fedora.files["/etc/os-release"] = "ID=fedora\n";
    const auto dnf = ContainerManager(fedora.host()).check_podman();
    check("Fedora is told to install crun with dnf",
          dnf && lemon::utils::SetupReport{{*dnf}}.action() == "sudo dnf install crun");

    FakeHost rootful = runc;
    rootful.oci_runtime = "runc";
    rootful.podman_rootless = "false";
    check("rootful Podman on runc passes, since it adds the render group by ID",
          !ContainerManager(rootful.host()).check_podman());

    FakeHost own_socket = runc;
    own_socket.oci_runtime = "runc";
    own_socket.env = {{"CONTAINER_HOST", "unix:///run/user/1000/podman/podman.sock"},
                      {"XDG_RUNTIME_DIR", "/run/user/1000"}};
    const auto api = ContainerManager(own_socket.host()).check_podman();
    check("the account's podman.socket on runc asks for crun", api && api->id == "oci_runtime");
}

void test_helpers() {
    check("characters a container name rejects are escaped as _ plus hex",
          ContainerManager::container_name("13305", "llamacpp", "nathanw", "user.My Model:Q4/x") ==
              "lemonade-13305-llamacpp-nathanw-user.My_20Model_3aQ4_2fx");
    check("names that differ only in rejected characters keep distinct containers",
          ContainerManager::container_name("13305", "llamacpp", "nathanw", "user.foo/bar") !=
                  ContainerManager::container_name("13305", "llamacpp", "nathanw", "user.foo-bar") &&
              ContainerManager::container_name("13305", "llamacpp", "nathanw", "user.foo_2fbar") ==
                  "lemonade-13305-llamacpp-nathanw-user.foo__2fbar");
    check("two lemond instances loading the same model get distinct containers",
          ContainerManager::container_name("13305", "llamacpp", "nathanw", "Qwen3-4B-GGUF") !=
              ContainerManager::container_name("13306", "llamacpp", "nathanw", "Qwen3-4B-GGUF"));

    FakeHost ubuntu;
    check("a host without SELinux passes the SELinux check",
          !ContainerManager(ubuntu.host()).check_selinux());
    FakeHost fedora;
    fedora.files["/sys/fs/selinux/enforce"] = "1";
    fedora.files["/sys/fs/selinux/booleans/container_use_devices"] = "0 0";
    const auto selinux = ContainerManager(fedora.host()).check_selinux();
    check("enforcing SELinux without container_use_devices fails with its fix",
          selinux &&
              selinux->message ==
                  "SELinux is enforcing and the container_use_devices boolean is off" &&
              selinux->steps ==
                  std::vector<std::string>({"sudo setsebool -P container_use_devices 1"}));
    fedora.files["/sys/fs/selinux/booleans/container_use_devices"] = "1 1";
    check("the SELinux check passes once container_use_devices is on",
          !ContainerManager(fedora.host()).check_selinux());
    fedora.files["/sys/fs/selinux/booleans/container_use_devices"] = "0 0";
    fedora.files["/sys/fs/selinux/enforce"] = "0";
    check("permissive SELinux passes the SELinux check",
          !ContainerManager(fedora.host()).check_selinux());

    check("other publishers are refused",
          !ContainerManager::allowed_image("docker.io/library/ubuntu") &&
              !ContainerManager::allowed_image("docker.io/kyuz0x/image") &&
              !ContainerManager::allowed_image("docker.io/kyuz0/"));

    const std::string digest =
        "sha256:e79f876de3be84200172552b7abe5686a230f1fce02caa65dbba90372e3255c6";
    const auto pin = lemon::backends::parse_container_pin("vulkan-radv-performance@" + digest);
    check("a pin splits into tag and digest",
          pin && pin->tag == "vulkan-radv-performance" && pin->digest == digest);
    check("a pin needs a full sha256 digest",
          !lemon::backends::parse_container_pin("vulkan-radv-performance@sha256:e79f") &&
              !lemon::backends::parse_container_pin("vulkan-radv-performance") &&
              !lemon::backends::parse_container_pin("b0001"));
}

#ifndef _WIN32
// A Hugging Face cache, where each snapshot file links into blobs/.
void test_model_mounts() {
    namespace fs = std::filesystem;
    const fs::path repo = fs::temp_directory_path() / "lemonade-test-model-mounts";
    fs::remove_all(repo);
    const fs::path blobs = repo / "blobs";
    const fs::path snapshot = repo / "snapshots" / "abc";
    fs::create_directories(blobs);
    fs::create_directories(snapshot / "tokenizer" / "extra");
    for (const char* blob : {"b1", "b2", "b3"}) std::ofstream(blobs / blob) << blob;
    fs::create_symlink("../../blobs/b1", snapshot / "model.hgn");
    fs::create_symlink("../../../blobs/b2", snapshot / "tokenizer" / "vocab.json");
    fs::create_symlink("../../../../blobs/b3", snapshot / "tokenizer" / "extra" / "merges.txt");
    const std::string real_blobs = fs::canonical(blobs).string();

    const auto file = ContainerManager::model_mounts((snapshot / "model.hgn").string(),
                                                     "/mnt/models/model.hgn");
    check("a linked file mounts its blob",
          file.size() == 1 && file[0].source == real_blobs + "/b1" &&
              file[0].destination == "/mnt/models/model.hgn");

    const auto dir = ContainerManager::model_mounts((snapshot / "tokenizer").string(),
                                                    "/mnt/models/tokenizer");
    check("a directory mounts each file inside it, at the same relative path",
          dir.size() == 2 && dir[0].source == real_blobs + "/b3" &&
              dir[0].destination == "/mnt/models/tokenizer/extra/merges.txt" &&
              dir[1].source == real_blobs + "/b2" &&
              dir[1].destination == "/mnt/models/tokenizer/vocab.json");

    bool threw = false;
    try {
        ContainerManager::model_mounts((repo / "missing").string(), "/mnt/models/missing");
    } catch (const std::exception&) {
        threw = true;
    }
    check("a missing model file is refused", threw);
    fs::remove_all(repo);
}
#endif

void test_halogen_kernel_check() {
    // A Strix Halo on Linux 7.0: SVM set, cwsr_size = 16384 + 40 * 479232.
    const std::string node = "gfx_target_version 110501\ncapability 671326848\n"
                             "simd_count 80\nctl_stack_size 16384\ncwsr_size 19185664\n";
    check("Halogen passes a kernel with SVM and the gfx1151 fixes",
          !lemon::backends::halogen::check_kfd_node(node));
    const auto no_svm = lemon::backends::halogen::check_kfd_node(
        "capability 537109120\nsimd_count 80\nctl_stack_size 16384\ncwsr_size 19185664\n");
    check("Halogen refuses a kernel built without SVM",
          no_svm && no_svm->steps == std::vector<std::string>(
                        {"Install Linux 6.18.4 or newer, built with CONFIG_HSA_AMD_SVM"}));
    const auto old_cwsr = lemon::backends::halogen::check_kfd_node(
        "capability 671326848\nsimd_count 80\nctl_stack_size 16384\ncwsr_size 19169280\n");
    check("Halogen refuses a kernel whose CWSR size predates the gfx1151 fixes",
          old_cwsr && old_cwsr->steps ==
                          std::vector<std::string>({"Install Linux 6.18.4 or newer"}));
    check("Halogen refuses a kernel too old to report the CWSR sizes",
          lemon::backends::halogen::check_kfd_node("capability 671326848\nsimd_count 80\n")
              .has_value());
}

void test_gufo_argv() {
    const std::string argv = join(lemon::backends::gufo::build_server_argv(
        "Qwen3.8-27B-Gufo", "/mnt/models/target.gguf", "/mnt/models/mmproj-BF16.gguf",
        "/mnt/models/draft.gguf", "dflash2", 8192, 8001, "--think off"));
    check("gufo serves the target under Lemonade's model name, with its projector and its "
          "DFlash2 draft",
          argv == "serve llm --model /mnt/models/target.gguf --served-model-name "
                  "Qwen3.8-27B-Gufo --context 8192 --host 0.0.0.0 --port 8001 --mmproj "
                  "/mnt/models/mmproj-BF16.gguf --speculative dflash2 --dflash-model "
                  "/mnt/models/draft.gguf --think off");
    check("gufo passes an MTP draft through --mtp-model and a DSpark draft through --dspark-model",
          join(lemon::backends::gufo::build_server_argv("m", "t.gguf", "", "d.gguf", "mtp", 0,
                                                        1, ""))
                      .find("--speculative mtp --mtp-model d.gguf") != std::string::npos &&
              join(lemon::backends::gufo::build_server_argv("m", "t.gguf", "", "d.gguf",
                                                            "dspark", 0, 1, ""))
                      .find("--speculative dspark --dspark-model d.gguf") != std::string::npos);
    using lemon::backends::gufo::speculative_mode;
    check("gufo reads a draft's mode from its GGUF architecture",
          speculative_mode("dflash", "qwen35") == "dflash2" &&
              speculative_mode("deepseek4-dspark", "deepseek4") == "dspark" &&
              speculative_mode("qwen4exp", "qwen4exp") == "mtp");
    bool threw = false;
    try {
        speculative_mode("llama", "qwen35");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("gufo refuses a draft of an architecture it cannot run", threw);
    threw = false;
    try {
        lemon::backends::gufo::build_server_argv("m", "t.gguf", "", "", "", 0, 1, "--port 9");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("gufo_args cannot move the port", threw);
}

void test_ds4_memory_plan() {
    using lemon::backends::ds4::memory_plan;
    // The IQ2XXS model (80.8 GiB) with its DSpark support file (5.6 GiB).
    const auto small_pool = memory_plan(62.0, 86.4);
    check("ds4 streams on a 62 GiB pool with the expert, model and free budgets it was tuned for",
          !small_pool.resident && small_pool.expert_cache_gb == 24 &&
              small_pool.model_cache_gb == 12 && small_pool.free_reserve_gb == 37);
    const auto large_pool = memory_plan(115.0, 86.4);
    check("ds4 runs resident on a 115 GiB pool and leaves 16 GiB free",
          large_pool.resident && large_pool.expert_cache_gb == 0 &&
              large_pool.free_reserve_gb == 16);
    check("ds4 streams a model that would leave less than 16 GiB free",
          !memory_plan(115.0, 100.0).resident);
}

void test_descriptors_and_pins() {
    std::ifstream in(BACKEND_VERSIONS_JSON_PATH);
    const nlohmann::json versions = nlohmann::json::parse(in);
    for (const auto* desc : lemon::backends::all_descriptors()) {
        for (const auto& row : desc->support) {
            const bool container = desc->container_for(row.backend) != nullptr;
            check(desc->recipe + ":" + row.backend + " is labeled container exactly when it has a policy",
                  container == (row.format == lemon::BackendFormat::Container));
        }
        for (const auto& [backend, policy] : desc->containers) {
            const std::string id = desc->recipe + ":" + backend;
            check(id + " pulls an allowed image",
                  ContainerManager::allowed_image(policy.image));
            const bool pinned = versions.contains(desc->recipe) &&
                                versions[desc->recipe].contains(backend) &&
                                versions[desc->recipe][backend].is_string();
            check(id + " is pinned to <tag>@<digest>",
                  pinned && lemon::backends::parse_container_pin(
                                versions[desc->recipe][backend].get<std::string>()));
        }
    }
}

}  // namespace

// lemond in the Lemonade image, driving the host account's Podman through its
// socket mounted at /run/podman/podman.sock.
FakeHost contained_host() {
    FakeHost host;
    host.tools = {"podman"};
    host.account = "lemonade";
    host.env = {{"CONTAINER_HOST", "unix:///run/podman/podman.sock"},
                {"XDG_RUNTIME_DIR", "/run/lemonade"}};
    host.paths = {"/run/.containerenv", "/run/podman/podman.sock"};
    const std::string id(64, 'a');
    host.files["/proc/self/mountinfo"] =
        "1 2 0:1 /home/user/.local/share/containers/storage/overlay-containers/" + id +
        "/userdata/hostname /etc/hostname rw - ext4 /dev/x rw\n";
    host.files["/etc/hostname"] = "lemonade-box\n";
    host.containers[id] =
        id + "|[{\"Type\":\"volume\",\"Name\":\"lemonade-cache\","
             "\"Source\":\"/home/user/.local/share/containers/storage/volumes/lemonade-cache/_data\","
             "\"Destination\":\"/opt/lemonade/.cache/huggingface\"},"
             "{\"Type\":\"bind\",\"Source\":\"/srv/models\",\"Destination\":\"/models\"},"
             "{\"Type\":\"bind\",\"Source\":\"/run/user/1000/podman/podman.sock\","
             "\"Destination\":\"/run/podman/podman.sock\"}]"
             "|{\"podman\":{\"IPAddress\":\"10.88.0.5\"},\"lemonade-backends\":{\"IPAddress\":\"10.89.0.2\",\"GlobalIPv6Address\":\"\"}}";
    host.internal_networks = {{"podman", "false"}, {"lemonade-backends", "true"}};
    return host;
}

void test_in_container() {
    const std::string id(64, 'a');
    FakeHost host = contained_host();
    const ContainerManager manager(host.host());
    check("lemond finds its own container through the mounted socket",
          manager.in_container() && manager.own_container() == id);
    check("a path in a named volume maps to the volume's directory on the host",
          manager.host_path("/opt/lemonade/.cache/huggingface/hub/blobs/x") ==
              "/home/user/.local/share/containers/storage/volumes/lemonade-cache/_data/hub/blobs/x");
    check("a path in a bind mount maps to the bind's source",
          manager.host_path("/models/a.gguf") == "/srv/models/a.gguf");
    bool refused = false;
    try {
        manager.host_path("/opt/lemonade/llama/a.gguf");
    } catch (const std::runtime_error&) {
        refused = true;
    }
    check("a path on no mount of lemond's container is refused", refused);
    check("a sibling mount's name prefix does not count as holding a path", [&] {
        try {
            manager.host_path("/modelsx/a.gguf");
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }());

    check("the backends' network is the internal one lemond's container is on",
          manager.backend_network() == "lemonade-backends");

    ContainerRunSpec spec = example_spec();
    spec.network = manager.backend_network();
    spec.pid = "container:" + id;
    spec.mounts = {{"/opt/lemonade/.cache/huggingface/hub/blobs/x",
                    "/mnt/models/Qwen3-4B-Q4_K_M.gguf"}};
    const std::string command = join(manager.run_command(spec));
    check("a backend joins the internal network and lemond's PID namespace, publishes no port "
          "and mounts host paths",
          command.find(" --network=lemonade-backends --pid=container:" + id + " ") != std::string::npos &&
              command.find(" --init") == std::string::npos &&
              command.find(" -p ") == std::string::npos &&
              command.find(" -v /home/user/.local/share/containers/storage/volumes/"
                           "lemonade-cache/_data/hub/blobs/x:/mnt/models/") != std::string::npos &&
              command.find(" --annotation run.oci.keep_original_groups=1") != std::string::npos);

    check("in a container, the setup checks pass once the host's tool finds it",
          manager.check_host().empty() && !manager.check_podman());

    FakeHost hostname_only = contained_host();
    hostname_only.files.erase("/proc/self/mountinfo");
    hostname_only.containers = {{"lemonade-box", "bbbb|[]|null"}};
    check("lemond falls back to its hostname to find its container",
          ContainerManager(hostname_only.host()).own_container() == "bbbb");

    FakeHost no_internal = contained_host();
    no_internal.internal_networks = {};
    const auto network_failures = ContainerManager(no_internal.host()).check_host();
    check("with no internal network, the failure says to create one and start lemond on it",
          network_failures.size() == 1 && network_failures[0].id == "backend_network" &&
              network_failures[0].steps[0] ==
                  "podman network create --internal --disable-dns lemonade-backends" &&
              network_failures[0].steps[1].find("--network podman --network lemonade-backends") !=
                  std::string::npos);

    FakeHost pasta = contained_host();
    pasta.containers[id] = id + "|[]|null";
    check("under pasta, lemond is on no internal network",
          ContainerManager(pasta.host()).backend_network().empty());

    FakeHost two_internal = contained_host();
    two_internal.containers[id] =
        id + "|[]|{\"aaa-internal\":{\"IPAddress\":\"10.90.0.2\"},"
             "\"lemonade-backends\":{\"IPAddress\":\"10.89.0.2\"}}";
    two_internal.internal_networks = {{"aaa-internal", "true"}, {"lemonade-backends", "true"}};
    check("of several internal networks, lemonade-backends is the backends' network",
          ContainerManager(two_internal.host()).backend_network() == "lemonade-backends");

    FakeHost no_socket = contained_host();
    no_socket.containers.clear();
    no_socket.podman_info_exit = 125;
    const auto failures = ContainerManager(no_socket.host()).check_host();
    check("with no socket mounted, the one failure says to mount it",
          failures.size() == 1 && failures[0].id == "own_container" &&
              failures[0].steps[0].find("docker/#container-backends") != std::string::npos);

    FakeHost on_host;
    on_host.tools = {"podman"};
    const ContainerManager host_manager(on_host.host());
    check("on the host, lemond is in no container and runs no inspect",
          !host_manager.in_container() && host_manager.own_container().empty() &&
              on_host.commands == 0);
    check("container names take lemond's container ID as the instance",
          ContainerManager::container_name("aaaaaaaaaaaa", "llamacpp", "nathanw", "m") ==
              "lemonade-aaaaaaaaaaaa-llamacpp-nathanw-m");
}

// lemond in the Lemonade image, driving the host's Docker through
// /var/run/docker.sock. The image carries both clients and sets CONTAINER_HOST.
void test_in_container_docker() {
    const std::string id(64, 'd');
    FakeHost host;
    host.tools = {"podman", "docker"};
    host.account = "lemonade";
    host.env = {{"CONTAINER_HOST", "unix:///run/podman/podman.sock"}};
    host.paths = {"/.dockerenv", "/var/run/docker.sock"};
    host.files["/proc/self/mountinfo"] =
        "1 2 0:1 /var/lib/docker/containers/" + id + "/hostname /etc/hostname rw - ext4 /dev/x rw\n";
    host.containers[id] =
        id + "|[{\"Type\":\"volume\",\"Name\":\"lemonade-cache\","
             "\"Source\":\"/var/lib/docker/volumes/lemonade-cache/_data\","
             "\"Destination\":\"/opt/lemonade/.cache/huggingface\"}]"
             "|{\"bridge\":{\"IPAddress\":\"172.17.0.2\"},\"lemonade-backends\":{\"IPAddress\":\"172.18.0.3\"}}";
    host.internal_networks = {{"lemonade-backends", "true"}};
    const ContainerManager manager(host.host());
    check("in a container, the tool is the one whose socket is mounted",
          manager.tool() == lemon::utils::ContainerTool::Docker && manager.own_container() == id);

    const auto failures = manager.check_host();
    check("Docker in a container needs the host's render group ID",
          failures.size() == 1 && failures[0].id == "render_group");

    FakeHost from_env = host;
    from_env.env["LEMONADE_RENDER_GID"] = "990";
    const ContainerManager env_manager(from_env.host());
    ContainerRunSpec spec = example_spec();
    spec.network = "lemonade-backends";
    spec.pid = "container:" + id;
    spec.mounts = {{"/opt/lemonade/.cache/huggingface/m.gguf", "/mnt/models/m.gguf"}};
    const std::string command = join(env_manager.run_command(spec));
    check("docker run takes LEMONADE_RENDER_GID, not the image's render group",
          env_manager.check_host().empty() && command.find(" --group-add 990 ") != std::string::npos &&
              command.find(" -v /var/lib/docker/volumes/lemonade-cache/_data/m.gguf:") !=
                  std::string::npos &&
              command.rfind("docker run ", 0) == 0);

    FakeHost from_device = host;
    from_device.gpu_group = "992";
    check("without LEMONADE_RENDER_GID, the GPU device nodes' owner is the render group",
          ContainerManager(from_device.host()).check_host().empty() &&
              join(ContainerManager(from_device.host()).run_command(spec)).find(" --group-add 992 ") !=
                  std::string::npos);

    FakeHost no_internal = from_env;
    no_internal.internal_networks = {};
    const auto network_failures = ContainerManager(no_internal.host()).check_host();
    check("Docker's step puts lemond on a user-defined network, since Docker refuses its default "
          "bridge beside one",
          network_failures.size() == 1 && network_failures[0].steps.size() == 3 &&
              network_failures[0].steps[1] == "docker network create lemonade" &&
              network_failures[0].steps[2].find("--network lemonade --network lemonade-backends") !=
                  std::string::npos);

    FakeHost neither = host;
    neither.paths = {"/.dockerenv"};
    check("with no socket mounted in a container, Podman stays the default",
          ContainerManager(neither.host()).tool() == lemon::utils::ContainerTool::Podman);
}

#ifdef __linux__
// A connection on lemond's address on the backends' network is refused unless
// it comes from that same address, as rootless Podman's port forwarder does.
void test_refused_connections() {
    const auto loopback = [](const char* text) {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        ::inet_pton(AF_INET, text, &address.sin_addr);
        return address;
    };
    const auto failed = [](const char* step) {
        check(std::string("refused-connection setup: ") + step + " failed: " + std::strerror(errno),
              false);
    };
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return failed("socket");
    sockaddr_in address = loopback("127.0.0.1");
    socklen_t length = sizeof(address);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 4) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(listener);
        return failed("listen on 127.0.0.1");
    }
    // The connection's verdict, or nullopt when a step fails.
    const auto accept_from = [&](const char* source) -> std::optional<bool> {
        const int client = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client < 0) {
            failed("client socket");
            return std::nullopt;
        }
        sockaddr_in from = loopback(source);
        if (::bind(client, reinterpret_cast<sockaddr*>(&from), sizeof(from)) != 0 ||
            ::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            failed((std::string("connect from ") + source).c_str());
            ::close(client);
            return std::nullopt;
        }
        const int accepted = ::accept(listener, nullptr, nullptr);
        ::close(client);
        if (accepted < 0) {
            failed("accept");
            return std::nullopt;
        }
        const bool refused = lemon::utils::is_refused_connection(accepted);
        ::close(accepted);
        return refused;
    };

    const auto unrefused = accept_from("127.0.0.2");
    if (unrefused) check("with no refused address, every connection is answered", !*unrefused);
    lemon::utils::set_refused_local_addresses({"127.0.0.1"});
    const auto other = accept_from("127.0.0.2");
    if (other) check("a connection from another address on lemond's backends' address is refused", *other);
    const auto own = accept_from("127.0.0.1");
    if (own) {
        check("a connection from lemond's own address, as a port forwarder makes, is answered", !*own);
    }
    lemon::utils::set_refused_local_addresses({});
    ::close(listener);
}
#endif

int main() {
    test_run_command();
    test_tool_choice_and_setup();
    test_system_service_setup();
    test_create_network();
    test_rootful_service();
    test_rootful_cni();
    test_install_commands();
    test_oci_runtime();
    test_helpers();
    test_in_container();
    test_in_container_docker();
#ifdef __linux__
    test_refused_connections();
#endif
#ifndef _WIN32
    test_model_mounts();
#endif

    test_halogen_kernel_check();
    test_gufo_argv();
    test_ds4_memory_plan();
    test_descriptors_and_pins();

    if (failures == 0) {
        std::printf("\nAll container manager checks passed.\n");
        return 0;
    }
    std::printf("\n%d container manager check(s) failed.\n", failures);
    return 1;
}
