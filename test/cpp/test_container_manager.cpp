// Asserts the container backends spec's Command Contract and setup checks
// against ContainerManager, with a fake host in place of Podman and Docker, and
// checks every container backend's descriptor and pin.

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/container_manager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

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
    // What Podman prints on stderr, which reaches `output` only when captured.
    std::string stderr_noise =
        "time=\"2026-10-17T00:57:31Z\" level=warning msg=\"Falling back to --cgroup-manager=cgroupfs\"\n";
    std::map<std::string, std::string> files = {{"/etc/os-release", "ID=ubuntu\nID_LIKE=debian\n"}};
    std::map<std::string, std::string> env;
    bool root = false;
    // How many commands the manager has run on the host.
    mutable int commands = 0;
    mutable std::vector<std::vector<std::string>> runs;

    ContainerHost host() const {
        ContainerHost h;
        h.on_path = [this](const std::string& name) { return tools.count(name) > 0; };
        h.gpu_devices_accessible = [this]() { return gpu_access; };
        h.account = [this]() { return account; };
        h.group_id = [](const std::string& group) {
            return group == "video" ? std::string("44") : std::string("990");
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
                result.output = podman_rootless + " " + network_backend + "\n";
            } else if (argv.size() > 1 && argv[1] == "network") {
                result.exit_code = 0;
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
    spec.name = "lemonade-llamacpp-nathanw-Qwen3-4B-GGUF";
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
        " run --rm --init --name lemonade-llamacpp-nathanw-Qwen3-4B-GGUF"
        " --label ai.lemonade"
        " --label ai.lemonade.recipe=llamacpp"
        " --label ai.lemonade.backend=nathanw"
        " --label ai.lemonade.model=Qwen3-4B-GGUF"
        " --label ai.lemonade.port=8001"
        " --cap-drop=all --security-opt=no-new-privileges"
        " --pull=never --network=lemonade-llamacpp-nathanw-Qwen3-4B-GGUF"
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

    FakeHost docker;
    docker.tools = {"docker"};
    const std::string docker_command = join(ContainerManager(docker.host()).run_command(example_spec()));
    check("docker run adds the host's video and render IDs and publishes no port",
          docker_command == "docker" + common + " --group-add 44 --group-add 990" +
                                mount_and_home + image_and_program);

    FakeHost rootful;
    rootful.tools = {"podman"};
    rootful.podman_rootless = "false";
    const ContainerManager rootful_manager(rootful.host());
    check("rootful podman run takes Docker's group IDs and publishes no port",
          !rootful_manager.rootless_podman() &&
              join(rootful_manager.run_command(example_spec())) ==
                  "podman" + common + " --group-add 44 --group-add 990" + mount_and_home +
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
              "1. sudo apt install podman\n2. sudo usermod -aG video,render $USER\n"
              "3. Log out and back in");
    check("error text lists the problems, then the steps",
          fresh.text() ==
              "\n  - podman is not on PATH\n  - Account cannot open the GPU devices\n"
              "To fix it:\n  1. sudo apt install podman\n"
              "  2. sudo usermod -aG video,render $USER\n  3. Log out and back in");

    FakeHost both;
    both.tools = {"podman", "docker"};
    const lemon::utils::SetupReport groups{ContainerManager(both.host()).check_host()};
    check("Podman needs the account to open the GPU devices",
          groups.ids() == std::vector<std::string>({"device_groups"}) &&
              groups.message() == "Account cannot open the GPU devices" &&
              groups.action() == "1. sudo usermod -aG video,render $USER\n"
                                 "2. Log out and back in");
    both.gpu_access = true;
    check("Podman passes once the account can open the GPU devices", ContainerManager(both.host()).check_host().empty());

    FakeHost remote;
    remote.tools = {"podman"};
    remote.env["CONTAINER_HOST"] = "unix:///run/podman/podman.sock";
    check("before podman info runs, Podman behind CONTAINER_HOST needs neither group",
          ContainerManager(remote.host()).check_host().empty() && remote.commands == 0);
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
              "1. sudo usermod -aG video,render $USER\n2. sudo usermod -aG docker $USER\n"
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
              groups.action() == "1. sudo usermod -aG video,render lemonade\n"
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
    const auto command = ContainerManager::podman_install_command;
    check("Rocky Linux matches fedora through ID_LIKE",
          command("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n") == "sudo dnf install podman");
    check("Fedora matches on ID", command("ID=fedora\n") == "sudo dnf install podman");
    check("an unknown distribution gets the generic step",
          command("ID=gentoo\n") == "Install Podman with the host's package manager");
}

void test_helpers() {
    check("characters a container name rejects are escaped as _ plus hex",
          ContainerManager::container_name("llamacpp", "nathanw", "user.My Model:Q4/x") ==
              "lemonade-llamacpp-nathanw-user.My_20Model_3aQ4_2fx");
    check("names that differ only in rejected characters keep distinct containers",
          ContainerManager::container_name("llamacpp", "nathanw", "user.foo/bar") !=
                  ContainerManager::container_name("llamacpp", "nathanw", "user.foo-bar") &&
              ContainerManager::container_name("llamacpp", "nathanw", "user.foo_2fbar") ==
                  "lemonade-llamacpp-nathanw-user.foo__2fbar");

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
void test_model_mount() {
    namespace fs = std::filesystem;
    const fs::path repo = fs::temp_directory_path() / "lemonade-test-model-mounts";
    fs::remove_all(repo);
    const fs::path blobs = repo / "blobs";
    const fs::path snapshot = repo / "snapshots" / "abc";
    fs::create_directories(blobs);
    fs::create_directories(snapshot);
    std::ofstream(blobs / "b1") << "b1";
    fs::create_symlink("../../blobs/b1", snapshot / "model.gguf");

    const auto mount = ContainerManager::model_mount((snapshot / "model.gguf").string(),
                                                     "/mnt/models/model.gguf");
    check("a linked file mounts its blob",
          mount.source == fs::canonical(blobs / "b1").string() &&
              mount.destination == "/mnt/models/model.gguf");

    bool threw = false;
    try {
        ContainerManager::model_mount((repo / "missing").string(), "/mnt/models/missing");
    } catch (const std::exception&) {
        threw = true;
    }
    check("a missing model file is refused", threw);
    fs::remove_all(repo);
}
#endif

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

int main() {
    test_run_command();
    test_tool_choice_and_setup();
    test_system_service_setup();
    test_create_network();
    test_rootful_service();
    test_rootful_cni();
    test_install_commands();
    test_helpers();
#ifndef _WIN32
    test_model_mount();
#endif
    test_descriptors_and_pins();

    if (failures == 0) {
        std::printf("\nAll container manager checks passed.\n");
        return 0;
    }
    std::printf("\n%d container manager check(s) failed.\n", failures);
    return 1;
}
