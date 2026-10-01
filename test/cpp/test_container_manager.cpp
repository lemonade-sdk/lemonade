// Asserts the container backends spec's Command Contract and setup checks
// against ContainerManager, with a fake host in place of Podman and Docker, and
// checks every container backend's descriptor and pin.

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/container_manager.h"

#include <cstdio>
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
    std::map<std::string, std::string> files = {{"/etc/os-release", "ID=ubuntu\nID_LIKE=debian\n"}};

    ContainerHost host() const {
        ContainerHost h;
        h.on_path = [this](const std::string& name) { return tools.count(name) > 0; };
        h.in_group = [this](const std::string& group) { return groups.count(group) > 0; };
        h.group_id = [](const std::string& group) {
            return group == "video" ? std::string("44") : std::string("990");
        };
        h.docker_socket_accepts = [this]() { return docker_socket; };
        h.read_file = [this](const std::string& path) {
            const auto it = files.find(path);
            return it == files.end() ? std::string() : it->second;
        };
        h.run = [](const std::vector<std::string>&, int) { return lemon::utils::CommandResult{}; };
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

    ContainerRunSpec ds4 = example_spec();
    ds4.devices = {"/dev/dri", "/dev/kfd"};
    ds4.cap_add = {"SYS_PTRACE"};
    ds4.env = {{"HIP_VISIBLE_DEVICES", "0"}, {"ROCBLAS_USE_HIPBLASLT", "1"}};
    const std::string policy_command = join(ContainerManager(podman.host()).run_command(ds4));
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
    const auto install = no_tool.check_setup();
    check("with no tool, the setup assistant says to install Podman",
          install && install->message == "podman is not on PATH" &&
              install->action == "sudo apt install podman");

    FakeHost both;
    both.tools = {"podman", "docker"};
    check("Podman is chosen when it is installed",
          ContainerManager(both.host()).tool() == lemon::utils::ContainerTool::Podman);

    both.groups = {"video"};
    const auto groups = ContainerManager(both.host()).check_setup();
    check("Podman needs the account in both video and render",
          groups && groups->message == "The user's account is not in both video and render" &&
              groups->action == "1. sudo usermod -aG video,render $USER\n"
                                "2. Log out and back in");
    both.groups = {"video", "render"};
    check("Podman passes with both groups", !ContainerManager(both.host()).check_setup());

    FakeHost docker;
    docker.tools = {"docker"};
    docker.docker_socket = false;
    check("Docker is chosen when Podman is not installed",
          ContainerManager(docker.host()).tool() == lemon::utils::ContainerTool::Docker);
    const auto refused = ContainerManager(docker.host()).check_setup();
    check("Docker needs the daemon to accept the account",
          refused && refused->message == "The Docker daemon refuses the user's account" &&
              refused->action == "1. sudo usermod -aG docker $USER\n2. Log out and back in");
    docker.docker_socket = true;
    check("Docker passes once the daemon accepts the account",
          !ContainerManager(docker.host()).check_setup());
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
              selinux->action == "sudo setsebool -P container_use_devices 1");
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
          ContainerManager::allowed_image("docker.io/kyuz0/strix-halo-ds4-toolbox"));
    check("ghcr.io/peonist-ai is allowed",
          ContainerManager::allowed_image("ghcr.io/peonist-ai/halogen-flash-server"));
    check("other publishers are refused",
          !ContainerManager::allowed_image("docker.io/library/ubuntu") &&
              !ContainerManager::allowed_image("docker.io/kyuz0x/image") &&
              !ContainerManager::allowed_image("docker.io/kyuz0/"));

    const std::string digest =
        "sha256:127783182e7cb721104b043be69508eafeae837dde1ad20e966e9db34a48f291";
    const auto pin = lemon::backends::parse_container_pin("rocm-10.0@" + digest);
    check("a pin splits into tag and digest",
          pin && pin->tag == "rocm-10.0" && pin->digest == digest);
    check("a pin needs a full sha256 digest",
          !lemon::backends::parse_container_pin("rocm-10.0@sha256:1277") &&
              !lemon::backends::parse_container_pin("rocm-10.0") &&
              !lemon::backends::parse_container_pin("b0001"));
}

void test_descriptors_and_pins() {
    std::ifstream in(BACKEND_VERSIONS_JSON_PATH);
    const nlohmann::json versions = nlohmann::json::parse(in);
    for (const auto* desc : lemon::backends::all_descriptors()) {
        for (const auto& [backend, labels] : desc->labels) {
            const bool container = desc->container_for(backend) != nullptr;
            check(desc->recipe + ":" + backend + " is labeled container exactly when it has a policy",
                  container == (labels.format == lemon::BackendFormat::Container));
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
    test_descriptors_and_pins();

    if (failures == 0) {
        std::printf("\nAll container manager checks passed.\n");
        return 0;
    }
    std::printf("\n%d container manager check(s) failed.\n", failures);
    return 1;
}
