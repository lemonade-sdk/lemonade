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
    std::set<std::string> groups;
    bool docker_socket = true;
    // `podman info`'s exit code and its Host.Security.Rootless answer.
    int podman_info_exit = 0;
    std::string podman_rootless = "true";
    // What Podman prints on stderr, which reaches `output` only when captured.
    std::string stderr_noise =
        "time=\"2026-10-17T00:57:31Z\" level=warning msg=\"Falling back to --cgroup-manager=cgroupfs\"\n";
    std::map<std::string, std::string> files = {{"/etc/os-release", "ID=ubuntu\nID_LIKE=debian\n"}};
    std::map<std::string, std::string> env;
    bool root = false;
    // How many commands the manager has run on the host.
    mutable int commands = 0;

    ContainerHost host() const {
        ContainerHost h;
        h.on_path = [this](const std::string& name) { return tools.count(name) > 0; };
        h.in_group = [this](const std::string& group) { return groups.count(group) > 0; };
        h.group_id = [](const std::string& group) {
            return group == "video" ? std::string("44") : std::string("990");
        };
        h.docker_socket_accepts = [this]() { return docker_socket; };
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
            lemon::utils::CommandResult result;
            if (argv.size() > 1 && argv[0] == "podman" && argv[1] == "info") {
                result = {podman_info_exit, podman_rootless + "\n"};
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
    check("seccomp keeps the tool's default profile",
          policy_command.find("seccomp") == std::string::npos);

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
    lemon::utils::SetupReport fresh{no_tool.check_host()};
    check("with no tool, every host step is reported at once: install Podman, then its groups",
          fresh.ids() == std::vector<std::string>({"container_tool", "device_groups"}));
    check("the host checks run no command before the user enables container backends",
          none.commands == 0);
    fresh.failures.push_back({"enable_containers", "enable_containers is false",
                              {"lemonade config set enable_containers=true"}, ""});
    check("each problem is on its own line",
          fresh.message() ==
              "podman is not on PATH\nAccount is not in both video and render\n"
              "enable_containers is false");
    check("the steps are numbered, consent after the host steps and logging out last",
          fresh.action() ==
              "1. sudo apt install podman\n2. sudo usermod -aG video,render $USER\n"
              "3. lemonade config set enable_containers=true\n4. Log out and back in");
    check("error text lists the problems, then the steps",
          fresh.text() ==
              "\n  - podman is not on PATH\n  - Account is not in both video and render\n"
              "  - enable_containers is false\nTo fix it:\n  1. sudo apt install podman\n"
              "  2. sudo usermod -aG video,render $USER\n"
              "  3. lemonade config set enable_containers=true\n  4. Log out and back in");
    const lemon::utils::SetupReport consent{{fresh.failures.back()}};
    check("a single step is not numbered",
          consent.action() == "lemonade config set enable_containers=true");

    FakeHost both;
    both.tools = {"podman", "docker"};
    check("Podman is chosen when it is installed",
          ContainerManager(both.host()).tool() == lemon::utils::ContainerTool::Podman);

    both.groups = {"video"};
    const lemon::utils::SetupReport groups{ContainerManager(both.host()).check_host()};
    check("Podman needs the account in both video and render",
          groups.ids() == std::vector<std::string>({"device_groups"}) &&
              groups.message() == "Account is not in both video and render" &&
              groups.action() == "1. sudo usermod -aG video,render $USER\n"
                                 "2. Log out and back in");
    both.groups = {"video", "render"};
    check("Podman passes with both groups", ContainerManager(both.host()).check_host().empty());
    check("rootless Podman passes podman info", !ContainerManager(both.host()).check_podman());

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
    broken.groups = {"video", "render"};
    broken.podman_info_exit = 125;
    const auto silent = ContainerManager(broken.host()).check_podman();
    check("Podman must answer podman info",
          silent && silent->id == "podman_info" && silent->message == "`podman info` fails" &&
              lemon::utils::SetupReport{{*silent}}.action() ==
                  "Run `podman info` and fix the error it prints");

    FakeHost docker;
    docker.tools = {"docker"};
    docker.docker_socket = false;
    check("Docker is chosen when Podman is not installed",
          ContainerManager(docker.host()).tool() == lemon::utils::ContainerTool::Docker);
    const lemon::utils::SetupReport refused{ContainerManager(docker.host()).check_host()};
    check("Docker needs the daemon to accept the account",
          refused.ids() == std::vector<std::string>({"docker_access"}) &&
              refused.message() == "The Docker daemon refuses the account" &&
              refused.action() == "1. sudo usermod -aG docker $USER\n2. Log out and back in");
    check("Docker needs no podman info", !ContainerManager(docker.host()).check_podman());
    docker.docker_socket = true;
    check("Docker passes once the daemon accepts the account",
          ContainerManager(docker.host()).check_host().empty());

    lemon::utils::SetupReport both_relogins{groups.failures};
    both_relogins.failures.push_back(refused.failures.front());
    check("logging out and back in is listed once, last",
          both_relogins.action() ==
              "1. sudo usermod -aG video,render $USER\n2. sudo usermod -aG docker $USER\n"
              "3. Log out and back in");
}

void test_install_commands() {
    const auto command = ContainerManager::podman_install_command;
    check("Ubuntu matches debian through ID_LIKE",
          command("ID=ubuntu\nID_LIKE=debian\n") == "sudo apt install podman");
    check("Rocky Linux matches fedora through ID_LIKE",
          command("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n") == "sudo dnf install podman");
    check("Fedora matches on ID", command("ID=fedora\n") == "sudo dnf install podman");
    check("CachyOS matches arch through ID_LIKE",
          command("ID=cachyos\nID_LIKE=arch\n") == "sudo pacman -S podman");
    check("an unknown distribution gets the generic step",
          command("ID=gentoo\n") == "Install Podman with the host's package manager");
}

void test_helpers() {
    check("the container name is lemonade-<recipe>-<backend>-<model>",
          ContainerManager::container_name("halogen", "rocm", "Qwen3.8-Flash-Next-Halogen") ==
              "lemonade-halogen-rocm-Qwen3.8-Flash-Next-Halogen");
    check("characters a container name rejects become dashes",
          ContainerManager::container_name("llamacpp", "nathanw", "user.My Model:Q4/x") ==
              "lemonade-llamacpp-nathanw-user.My-Model-Q4-x");

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

    check("gfx_target_version 110501 is gfx1151", ContainerManager::gfx_name(110501) == "gfx1151");
    check("gfx_target_version 90010 is gfx90a", ContainerManager::gfx_name(90010) == "gfx90a");
    check("the GPU index skips CPU nodes",
          ContainerManager::gpu_index({0, 110501}, "gfx1151") == "0");
    check("the GPU index counts other GPUs before the match",
          ContainerManager::gpu_index({0, 120001, 110501}, "gfx1151") == "1");
    check("no index when no GPU matches", ContainerManager::gpu_index({0, 120001}, "gfx1151").empty());

    check("docker.io/kyuz0 is allowed",
          ContainerManager::allowed_image("docker.io/kyuz0/amd-strix-halo-toolboxes"));
    check("ghcr.io/peonist-ai is allowed",
          ContainerManager::allowed_image("ghcr.io/peonist-ai/halogen-flash-server"));
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
            check(id + " lists its device nodes", !policy.devices.empty());
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
    test_install_commands();
    test_helpers();
#ifndef _WIN32
    test_model_mounts();
#endif
    test_descriptors_and_pins();

    if (failures == 0) {
        std::printf("\nAll container manager checks passed.\n");
        return 0;
    }
    std::printf("\n%d container manager check(s) failed.\n", failures);
    return 1;
}
