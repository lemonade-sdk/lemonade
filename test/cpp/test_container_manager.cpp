// Standalone test for ContainerManager.
// Build with: cmake --build --preset default --target test_container_manager
// Run with: ctest --test-dir build -R '^ContainerManagerTest$' --output-on-failure
//
// Every podman or docker invocation goes through an injected CommandRunner, so
// this exercises the whole layer with neither tool on the machine.

#include "lemon/utils/container_manager.h"

#include <iostream>
#include <string>
#include <vector>

using lemon::utils::CommandResult;
using lemon::utils::ContainerImage;
using lemon::utils::ContainerManager;
using lemon::utils::ContainerMount;
using lemon::utils::ContainerReadiness;
using lemon::utils::ContainerRunSpec;
using lemon::utils::ContainerTool;
using lemon::utils::HostTransport;

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
    if (condition) {
        std::cout << "PASS: " << label << std::endl;
    } else {
        std::cout << "FAIL: " << label << std::endl;
        ++failures;
    }
}

std::string join(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
        if (!out.empty()) out += " ";
        out += a;
    }
    return out;
}

bool contains(const std::vector<std::string>& args, const std::string& needle) {
    return join(args).find(needle) != std::string::npos;
}

// Index of `value` in args, or -1.
int index_of(const std::vector<std::string>& args, const std::string& value) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == value) return static_cast<int>(i);
    }
    return -1;
}

ContainerManager::Info podman_info() {
    ContainerManager::Info info;
    info.tool = ContainerTool::Podman;
    info.executable = "/usr/bin/podman";
    return info;
}

ContainerManager::Info docker_info() {
    ContainerManager::Info info;
    info.tool = ContainerTool::Docker;
    info.executable = "/usr/bin/docker";
    return info;
}

ContainerRunSpec sample_spec() {
    ContainerRunSpec spec;
    spec.name = "lemonade-rocmfpx-rocmfpx-8123";
    spec.image.repository = "docker.io/kyuz0/amd-strix-halo-toolboxes";
    spec.image.digest = "sha256:abc";
    spec.image.devices = {"/dev/dri", "/dev/kfd"};
    spec.labels = {{"ai.lemonade.recipe", "rocmfpx"},
                   {"ai.lemonade.backend", "rocmfpx"},
                   {"ai.lemonade.port", "8123"}};
    spec.groups = {"keep-groups"};
    spec.mounts.push_back({"/home/u/.cache/huggingface/hub/blobs/deadbeef",
                           "/mnt/models/model.gguf", true});
    spec.network = "lemonade-rocmfpx-rocmfpx-8123";
    spec.host_port = 8123;
    spec.container_port = 8123;
    spec.command = {"llama-server", "-m", "/mnt/models/model.gguf"};
    return spec;
}

// ---------------------------------------------------------------------------

// The run contract: what every container gets, and nothing more.
void test_run_args() {
    const auto args = ContainerManager::build_run_args(sample_spec(), ContainerTool::Podman);

    expect(args[0] == "run", "run args start with run");
    expect(contains(args, "--rm"), "container is removed on exit");
    expect(contains(args, "--init"), "a PID 1 reaper is installed");
    expect(contains(args, "--name lemonade-rocmfpx-rocmfpx-8123"), "container is named");
    expect(contains(args, "--label ai.lemonade "), "ownership label is set");
    expect(contains(args, "--label ai.lemonade.recipe=rocmfpx"), "recipe label is set");
    expect(contains(args, "--label ai.lemonade.backend=rocmfpx"), "backend label is set");
    expect(contains(args, "--label ai.lemonade.port=8123"), "port label is set");
    expect(contains(args, "--cap-drop=all"), "all capabilities dropped");
    expect(contains(args, "--security-opt=no-new-privileges"), "no new privileges");
    expect(contains(args, "--security-opt=label=disable"), "SELinux separation off");
    expect(!contains(args, "seccomp=unconfined"), "the tool's default seccomp profile applies");
    expect(contains(args, "--pull=never"), "run never pulls");
    expect(contains(args, "--network=lemonade-rocmfpx-rocmfpx-8123"),
           "container joins its own private network");
    expect(contains(args, "--device /dev/kfd"), "kfd passed through");
    expect(contains(args, "--device /dev/dri"), "dri passed through");
    expect(contains(args, "--group-add keep-groups"), "groups passed through");
    expect(!contains(args, "--ipc=host") && !contains(args, "--cap-add") &&
               !contains(args, "--ulimit"),
           "no extra access unless the entry asks for it");
    expect(contains(args, "--mount type=bind,src=/home/u/.cache/huggingface/hub/blobs/deadbeef,"
                          "destination=/mnt/models/model.gguf,ro"),
           "model blob bind-mounted read-only at /mnt/models");
    expect(contains(args, "--env HOME=/tmp"), "HOME points at a writable tmpfs");
    expect(contains(args, "-p 127.0.0.1:8123:8123"), "port published on loopback only");

    // The image must come last, immediately before the container command, or
    // the tool parses the server's argv as its own flags.
    const int image_idx = index_of(args, sample_spec().image.pinned_ref());
    expect(image_idx > 0, "image present");
    expect(args[static_cast<size_t>(image_idx) + 1] == "llama-server",
           "command follows the image");
    expect(args.back() == "/mnt/models/model.gguf", "command argv preserved in order");
}

void test_extra_access() {
    ContainerRunSpec spec = sample_spec();
    spec.image.cap_add = {"SYS_PTRACE"};
    spec.image.ipc_host = true;
    spec.image.memlock_unlimited = true;
    spec.image.env = {{"ROCBLAS_USE_HIPBLASLT", "1"}};
    spec.env = {{"HIP_VISIBLE_DEVICES", "0"}};
    const auto args = ContainerManager::build_run_args(spec, ContainerTool::Podman);
    expect(contains(args, "--ipc=host"), "ipc_host shares the host IPC namespace");
    expect(contains(args, "--ulimit memlock=-1:-1"), "memlock_unlimited lifts the limit");
    expect(contains(args, "--cap-add SYS_PTRACE"), "cap_add gives the capability back");
    const int drop = index_of(args, "--cap-drop=all");
    const int add = index_of(args, "--cap-add");
    expect(drop >= 0 && add > drop, "cap-add follows cap-drop so it survives");
    expect(contains(args, "--env ROCBLAS_USE_HIPBLASLT=1"), "the image's env applied");
    expect(contains(args, "--env HIP_VISIBLE_DEVICES=0"), "the run's own env applied");
}

void test_docker_is_reached_by_address() {
    ContainerRunSpec spec = sample_spec();
    spec.publish_port = false;
    const auto args = ContainerManager::build_run_args(spec, ContainerTool::Docker);
    expect(contains(args, "--network=lemonade-rocmfpx-rocmfpx-8123") && !contains(args, "-p "),
           "docker on an internal network publishes nothing");
}

void test_shared_network_namespace() {
    ContainerRunSpec spec = sample_spec();
    spec.network = "container:abc123";
    const auto args = ContainerManager::build_run_args(spec, ContainerTool::Podman);
    expect(contains(args, "--network=container:abc123"), "joins lemond's namespace");
    expect(!contains(args, "-p "), "no port published inside a shared namespace");
}

void test_volume_mounts() {
    ContainerRunSpec spec = sample_spec();
    ContainerMount volume;
    volume.host_path = "lemonade-cache";
    volume.container_path = "/mnt/models/model.gguf";
    volume.volume = true;
    volume.volume_subpath = "hub/blobs/deadbeef";
    spec.mounts = {volume};

    const auto docker_args = ContainerManager::build_run_args(spec, ContainerTool::Docker);
    expect(contains(docker_args, "--mount type=volume,src=lemonade-cache,destination="
                                 "/mnt/models/model.gguf,volume-subpath=hub/blobs/deadbeef,ro"),
           "docker spells a volume subpath as volume-subpath");
    const auto podman_args = ContainerManager::build_run_args(spec, ContainerTool::Podman);
    expect(contains(podman_args, "--mount type=volume,src=lemonade-cache,destination="
                                 "/mnt/models/model.gguf,subpath=hub/blobs/deadbeef,ro"),
           "podman spells a volume subpath as subpath");
}

void test_group_gids() {
    // "root" is gid 0 everywhere this runs.
    expect(ContainerManager::host_group_gid("root") == "0", "a known group resolves to its gid");
    expect(ContainerManager::host_group_gid("definitely-not-a-real-group-xyz").empty(),
           "an undefined group has no gid");
}

void test_image_refs() {
    ContainerImage image;
    image.repository = "docker.io/kyuz0/amd-strix-halo-toolboxes";
    image.tag = "rocm-10.0";
    image.digest = "sha256:deadbeef";

    expect(image.pinned_ref() == "docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:deadbeef",
           "pinned ref uses the digest, not the tag");
    expect(join(ContainerManager::build_pull_args(image)) ==
               "pull docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:deadbeef",
           "pull targets the digest");

    ContainerImage untagged;
    untagged.repository = "example/img";
    untagged.tag = "v1";
    expect(untagged.pinned_ref() == "example/img:v1", "no digest falls back to the tag");

    expect(join(ContainerManager::build_stop_args("lemonade-ds4-rocm-8123", 10)) ==
               "stop --time 10 lemonade-ds4-rocm-8123",
           "stop is by container name with a grace period");
}

void test_container_names() {
    expect(ContainerManager::container_name("llamacpp", "nathanw", 8001) ==
               "lemonade-llamacpp-nathanw-8001",
           "container name is lemonade-<recipe>-<backend>-<port>");
    expect(ContainerManager::container_name("llamacpp", "nathanw", 8001) !=
               ContainerManager::container_name("llamacpp", "nathanw", 8002),
           "two models on one backend get different containers");
    expect(ContainerManager::container_name("we/ird", "a b", 8001) == "lemonade-we-ird-a-b-8001",
           "unsafe characters are replaced");
    expect(std::string(ContainerManager::managed_label()) == "ai.lemonade",
           "managed label is what sweep filters on");
}

void test_parsers() {
    expect(ContainerManager::parse_version("Docker version 29.7.2, build a7dcaa6") == "29.7.2",
           "docker version parsed");
    expect(ContainerManager::parse_version("podman version 5.3.1") == "5.3.1",
           "podman version parsed");
    expect(ContainerManager::parse_version("garbage").empty(), "unparseable yields empty");

    // Real `docker images --digests --format '{{.Repository}}@{{.Digest}}'`
    // output: the repository is stored without the docker.io/ prefix.
    const std::string images_output =
        "kyuz0/amd-strix-halo-toolboxes@sha256:aaa\nother/image@sha256:bbb\n";
    const auto matched = ContainerManager::parse_repo_digests(
        images_output, "docker.io/kyuz0/amd-strix-halo-toolboxes");
    expect(matched.size() == 1 && matched[0] == "sha256:aaa",
           "digest matched with the docker.io prefix stripped");
    expect(ContainerManager::parse_repo_digests(images_output, "nope/nope").empty(),
           "no digest for an absent repository");

    // Backends of one recipe are different tags of the same repository, so a
    // repository can hold several digests at once.
    const std::string two_backends =
        "kyuz0/amd-strix-halo-toolboxes@sha256:aaa\n"
        "kyuz0/amd-strix-halo-toolboxes@sha256:bbb\n";
    const auto both = ContainerManager::parse_repo_digests(
        two_backends, "docker.io/kyuz0/amd-strix-halo-toolboxes");
    expect(both.size() == 2 && both[0] == "sha256:aaa" && both[1] == "sha256:bbb",
           "every digest of a repository is returned");
    expect(ContainerManager::parse_repo_digests("<no value>", "a/b").empty(),
           "an empty rendering is not a digest");
    expect(ContainerManager::parse_repo_digests("a/b@<none>", "a/b").empty(),
           "a tag-only image reports no digest");
}

void test_gpu_selection() {
    expect(ContainerManager::gfx_name_from_target_version(110501) == "gfx1151",
           "Strix Halo target version names gfx1151");
    expect(ContainerManager::gfx_name_from_target_version(120001) == "gfx1201",
           "R9700 target version names gfx1201");
    expect(ContainerManager::gfx_name_from_target_version(90010) == "gfx90a",
           "stepping renders as a hex digit");
    expect(ContainerManager::gfx_name_from_target_version(0).empty(),
           "a CPU node has no gfx name");

    // KFD lists the CPU node first; HIP counts GPUs only.
    const std::vector<int> topology = {0, 110000, 110501};
    expect(ContainerManager::pick_gpu_index(topology, "gfx1151") == "1",
           "GPU index skips CPU nodes");
    expect(ContainerManager::pick_gpu_index(topology, "gfx1100") == "0",
           "first GPU is index 0");
    expect(ContainerManager::pick_gpu_index(topology, "gfx1201").empty(),
           "an arch that is not present selects nothing");
    expect(ContainerManager::pick_gpu_index(topology, "").empty(), "no arch selects nothing");
}

void test_self_mount_parsing() {
    // `inspect --format {{json .Mounts}}` on lemond's own container.
    const std::string mounts =
        R"([{"Type":"volume","Name":"lemonade-cache","Source":"/var/lib/docker/volumes/lemonade-cache/_data","Destination":"/opt/lemonade/.cache/huggingface","RW":true},)"
        R"({"Type":"bind","Source":"/run/user/1000/podman/podman.sock","Destination":"/run/podman/podman.sock","RW":true},)"
        R"({"Type":"bind","Source":"/srv/models","Destination":"/models","RW":false}])";
    const auto parsed = ContainerManager::parse_self_mounts(mounts);
    expect(parsed.size() == 3, "every mount is parsed");
    expect(parsed[0].volume && parsed[0].host_path == "lemonade-cache" &&
               parsed[0].container_path == "/opt/lemonade/.cache/huggingface",
           "a named volume is addressed by name, not the tool's private path");
    expect(!parsed[1].volume && parsed[1].host_path == "/run/user/1000/podman/podman.sock",
           "a bind mount keeps its host source");
    expect(parsed[2].read_only, "read-only flag is carried");
    expect(ContainerManager::parse_self_mounts("not json").empty(), "garbage parses to nothing");

    // The whole-container form wraps the same array.
    const std::string whole = R"([{"Id":"abc","Mounts":)" + mounts + "}]";
    expect(ContainerManager::parse_self_mounts(whole).size() == 3,
           "the full inspect document is accepted too");
}

// --- behavior against a fake podman or docker -------------------------------

struct FakeTool {
    std::vector<std::pair<std::string, std::vector<std::string>>> calls;
    CommandResult next{0, ""};

    lemon::utils::CommandRunner runner() {
        return [this](const std::string& executable, const std::vector<std::string>& args,
                      int) -> CommandResult {
            calls.push_back({executable, args});
            if (!args.empty() && args[0] == "version") return {0, "9.9.9"};
            if (!args.empty() && args[0] == "ps") return {0, "lemonade-ds4-rocm-8123\nlemonade-x\n"};
            return next;
        };
    }
};

void test_sweep_filters_by_label() {
    FakeTool fake;
    ContainerManager manager(fake.runner());
    if (!manager.info()) {
        std::cout << "SKIP: no podman or docker on PATH, sweep round-trip not exercised"
                  << std::endl;
        return;
    }
    const int removed = manager.sweep_managed_containers();
    expect(removed == 2, "every labelled container is removed");
    bool filtered = false;
    for (const auto& [exe, args] : fake.calls) {
        if (!args.empty() && args[0] == "ps" && contains(args, "--filter label=ai.lemonade")) {
            filtered = true;
        }
    }
    expect(filtered, "sweep lists containers by the ownership label");
}

void test_run_command() {
    FakeTool fake;
    ContainerManager manager(fake.runner(), HostTransport::Native);
    if (!manager.info()) {
        std::cout << "SKIP: no podman or docker on PATH, run command not exercised" << std::endl;
        return;
    }
    const auto command = manager.run_command(sample_spec());
    expect(command[0] == manager.info()->executable && command[1] == "run",
           "the run command starts with the tool, then run");
    expect(command.back() == "/mnt/models/model.gguf", "the run command ends with the server argv");
}

void test_group_adds() {
    FakeTool fake;
    ContainerManager manager(fake.runner());
    expect(manager.group_adds({}).empty(), "no devices, no groups");
    if (!manager.info()) return;
    const auto groups = manager.group_adds({"/dev/dri"});
    if (manager.info()->tool == ContainerTool::Podman) {
        expect(groups == std::vector<std::string>{"keep-groups"},
               "podman passes the user's own groups through");
    } else {
        bool numeric = true;
        for (const auto& gid : groups) {
            numeric = numeric && gid.find_first_not_of("0123456789") == std::string::npos;
        }
        expect(numeric, "docker gets the host's numeric gids, never group names");
    }
}

void test_toolbox_invocation() {
    FakeTool fake;
    ContainerManager manager(fake.runner(), HostTransport::Toolbox);
    const auto [exe, prefix] = manager.invocation(podman_info());
    // Whether flatpak-spawn exists depends on the machine; either the host
    // prefix is applied or the tool is called directly.
    const bool spawns = exe.find("flatpak-spawn") != std::string::npos;
    expect((spawns && prefix.size() >= 2 && prefix[0] == "--host" && prefix[1] == "podman") ||
               (!spawns && exe == "/usr/bin/podman"),
           "toolbox transport runs the tool on the host when flatpak-spawn exists");
}

void test_native_invocation_is_bare() {
    FakeTool fake;
    ContainerManager manager(fake.runner(), HostTransport::Native);
    const auto [exe, prefix] = manager.invocation(docker_info());
    expect(exe == "/usr/bin/docker" && prefix.empty(), "native transport calls the tool as is");
}

void test_container_transport_readiness() {
    FakeTool fake;
    ContainerManager manager(fake.runner(), HostTransport::Container);
    if (!manager.info()) {
        std::cout << "SKIP: no podman or docker on PATH" << std::endl;
        return;
    }
    // `inspect` on this process's hostname fails, so lemond cannot see itself
    // and the readiness check must say so.
    fake.next = {1, "no such container"};
    const auto result = manager.check_readiness({"/dev/dri", "/dev/kfd"});
    expect(result.state == ContainerReadiness::NoHostMountMapping &&
               result.remediation_id == "engine-socket",
           "a container that cannot inspect itself is reported with the socket remediation");
}

void test_pull_requires_repository() {
    FakeTool fake;
    ContainerManager manager(fake.runner());
    bool threw = false;
    try {
        manager.pull(ContainerImage{});
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "pulling an empty image is an error");
}

void test_readiness_names_a_remediation() {
    FakeTool fake;
    ContainerManager manager(fake.runner());
    const auto result = manager.check_readiness({"/dev/dri", "/dev/kfd"});
    // Whether podman or docker exists depends on the machine; either way the
    // result carries a remediation id whenever it is not ready.
    expect(result.ok() || !result.remediation_id.empty(),
           "an unready host always names a remediation section");
}

}  // namespace

int main() {
    test_run_args();
    test_extra_access();
    test_docker_is_reached_by_address();
    test_shared_network_namespace();
    test_volume_mounts();
    test_group_gids();
    test_image_refs();
    test_container_names();
    test_parsers();
    test_gpu_selection();
    test_self_mount_parsing();
    test_sweep_filters_by_label();
    test_run_command();
    test_group_adds();
    test_toolbox_invocation();
    test_native_invocation_is_bare();
    test_container_transport_readiness();
    test_pull_requires_repository();
    test_readiness_names_a_remediation();

    if (failures == 0) {
        std::cout << "All container manager tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " container manager test(s) failed" << std::endl;
    return 1;
}
