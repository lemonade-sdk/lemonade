// Standalone test for ContainerManager: the Run Options, Command Prefix and
// Setup Assistant tables of the toolbox architecture spec.
// Build with: cmake --build --preset default --target test_container_manager
// Run with: ctest --test-dir build -R '^ContainerManagerTest$' --output-on-failure
//
// Fake podman and docker binaries on PATH make the tools "installed", and an
// injected CommandRunner answers every call, so neither tool is needed.

#include "lemon/utils/container_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using lemon::utils::CommandResult;
using lemon::utils::ContainerImage;
using lemon::utils::ContainerManager;
using lemon::utils::ContainerMount;
using lemon::utils::ContainerRunSpec;
using lemon::utils::ContainerTool;
using lemon::utils::HostTransport;

namespace fs = std::filesystem;

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

int count_of(const std::vector<std::string>& args, const std::string& value) {
    int count = 0;
    for (const auto& arg : args) count += arg == value ? 1 : 0;
    return count;
}

#ifndef _WIN32

// A directory holding executable stand-ins for podman and docker, put on PATH
// in place of the real one.
class FakePath {
public:
    explicit FakePath(const std::vector<std::string>& tools) {
        dir_ = fs::temp_directory_path() / ("lemonade-fake-path-" + std::to_string(::getpid()) +
                                            "-" + std::to_string(counter_++));
        fs::create_directories(dir_);
        for (const auto& tool : tools) {
            const fs::path path = dir_ / tool;
            std::ofstream(path) << "#!/bin/sh\nexit 0\n";
            fs::permissions(path, fs::perms::owner_all);
        }
        const char* old = std::getenv("PATH");
        saved_ = old ? old : "";
        ::setenv("PATH", dir_.c_str(), 1);
    }
    ~FakePath() {
        ::setenv("PATH", saved_.c_str(), 1);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

private:
    static inline int counter_ = 0;
    fs::path dir_;
    std::string saved_;
};

// Records every call and answers from a table keyed on the first argument.
struct FakeTool {
    std::vector<std::pair<std::string, std::vector<std::string>>> calls;
    std::map<std::string, CommandResult> answers;

    lemon::utils::CommandRunner runner() {
        return [this](const std::string& executable, const std::vector<std::string>& args,
                      int) -> CommandResult {
            calls.push_back({executable, args});
            std::string key = executable;
            for (size_t i = 0; i < args.size(); ++i) {
                const std::string& arg = args[i];
                if (arg == "--host" || arg == "--remote" || arg == "podman" || arg == "docker") {
                    continue;
                }
                key = arg;
                if (arg == "network" && i + 1 < args.size()) key += " " + args[i + 1];
                break;
            }
            if (executable == "snapctl" && args.size() >= 2) key = "snapctl " + args[1];
            const auto it = answers.find(key);
            return it != answers.end() ? it->second : CommandResult{0, ""};
        };
    }

    std::vector<std::string> calls_starting_with(const std::string& verb) const {
        std::vector<std::string> out;
        for (const auto& [exe, args] : calls) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--remote" || args[i] == "--host") continue;
                if (args[i] == verb) out.push_back(join(args));
                break;
            }
        }
        return out;
    }
};

#endif

ContainerManager::Info podman_info() {
    ContainerManager::Info info;
    info.tool = ContainerTool::Podman;
    info.executable = "/usr/bin/podman";
    return info;
}

// The spec's "Example: The Complete Command": Qwen3-4B-GGUF on llamacpp:nathanw
// with rootless Podman on a native host.
ContainerRunSpec nathanw_spec() {
    ContainerRunSpec spec;
    spec.name = "lemonade-llamacpp-nathanw-Qwen3-4B-GGUF";
    spec.image.repository = "docker.io/kyuz0/amd-strix-halo-toolboxes";
    spec.image.tag = "vulkan-radv-performance";
    spec.image.digest =
        "sha256:42630818d084f3fa712a06a8fd2259338361e52f1c36720cb3b75639686bac03";
    spec.image.devices = {"/dev/dri"};
    spec.labels = {{"ai.lemonade.recipe", "llamacpp"},
                   {"ai.lemonade.backend", "nathanw"},
                   {"ai.lemonade.model", "Qwen3-4B-GGUF"},
                   {"ai.lemonade.port", "8001"}};
    spec.mounts.push_back(
        {"/home/alice/.cache/huggingface/hub/models--unsloth--Qwen3-4B-GGUF/blobs/9a8c",
         "/mnt/models/Qwen3-4B-Q4_K_M.gguf", true});
    spec.network = "lemonade-llamacpp-nathanw-Qwen3-4B-GGUF";
    spec.port = 8001;
    spec.command = {"llama-server", "-m",     "/mnt/models/Qwen3-4B-Q4_K_M.gguf",
                    "--ctx-size",   "8192",   "--port",
                    "8001",         "--host", "0.0.0.0",
                    "--jinja",      "--metrics", "--parallel",
                    "1"};
    return spec;
}

// Appendix B: Qwen3.8-Flash-Next-Halogen on halogen:rocm.
ContainerRunSpec halogen_spec() {
    const std::string snapshot =
        "/home/alice/.cache/huggingface/hub/models--peonist-ai--halogen-qwen3.8-flash-next/"
        "snapshots/ac23b1b223b4e9192d27c22367d4dbacf2b595ef";
    ContainerRunSpec spec;
    spec.name = "lemonade-halogen-rocm-Qwen3.8-Flash-Next-Halogen";
    spec.image.repository = "ghcr.io/peonist-ai/halogen-flash-server";
    spec.image.tag = "latest";
    spec.image.digest =
        "sha256:6e626c979d536ab1edb07898e278be6686afd353758ea268817457f801d687dd";
    spec.image.devices = {"/dev/dri", "/dev/kfd"};
    spec.image.env = {{"HALOGEN_CTX", "262144"},
                      {"HALOGEN_KV_POOL_POSITIONS", "524288"},
                      {"HALOGEN_KV_SLOTS", "4"},
                      {"HALOGEN_PROMPT_CACHE", "2"}};
    spec.image.ipc_host = true;
    spec.image.memlock_unlimited = true;
    spec.labels = {{"ai.lemonade.recipe", "halogen"},
                   {"ai.lemonade.backend", "rocm"},
                   {"ai.lemonade.model", "Qwen3.8-Flash-Next-Halogen"},
                   {"ai.lemonade.port", "8001"}};
    for (const std::string file :
         {"qwen38-flash-next-w4b.hgn", "qwen38-flash-next-w4b.overlay.hgn", "tokenizer"}) {
        spec.mounts.push_back({snapshot + "/" + file, "/mnt/models/" + file, true});
    }
    spec.env = {{"HALOGEN_CHECKPOINT", "/mnt/models/qwen38-flash-next-w4b.hgn"},
                {"HALOGEN_CK_OVERLAY", "/mnt/models/qwen38-flash-next-w4b.overlay.hgn"},
                {"HALOGEN_TOKENIZER", "/mnt/models/tokenizer"},
                {"HALOGEN_API_PORT", "8001"},
                {"HIP_VISIBLE_DEVICES", "0"}};
    spec.network = "lemonade-halogen-rocm-Qwen3.8-Flash-Next-Halogen";
    spec.port = 8001;
    return spec;
}

// ---------------------------------------------------------------------------

void test_spec_example_command() {
    const std::vector<std::string> expected = {
        "run",
        "--rm",
        "--init",
        "--name",
        "lemonade-llamacpp-nathanw-Qwen3-4B-GGUF",
        "--label",
        "ai.lemonade",
        "--label",
        "ai.lemonade.recipe=llamacpp",
        "--label",
        "ai.lemonade.backend=nathanw",
        "--label",
        "ai.lemonade.model=Qwen3-4B-GGUF",
        "--label",
        "ai.lemonade.port=8001",
        "--cap-drop=all",
        "--security-opt=no-new-privileges",
        "--security-opt=label=disable",
        "--pull=never",
        "--network=lemonade-llamacpp-nathanw-Qwen3-4B-GGUF",
        "--device",
        "/dev/dri",
        "--group-add",
        "keep-groups",
        "--mount",
        "type=bind,src=/home/alice/.cache/huggingface/hub/models--unsloth--Qwen3-4B-GGUF/blobs/"
        "9a8c,destination=/mnt/models/Qwen3-4B-Q4_K_M.gguf,ro",
        "--env",
        "HOME=/tmp",
        "-p",
        "127.0.0.1:8001:8001",
        "docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:"
        "42630818d084f3fa712a06a8fd2259338361e52f1c36720cb3b75639686bac03",
        "llama-server",
        "-m",
        "/mnt/models/Qwen3-4B-Q4_K_M.gguf",
        "--ctx-size",
        "8192",
        "--port",
        "8001",
        "--host",
        "0.0.0.0",
        "--jinja",
        "--metrics",
        "--parallel",
        "1",
    };
    const auto args = ContainerManager::build_run_args(nathanw_spec(), ContainerTool::Podman);
    expect(args == expected, "Podman builds the spec's example command exactly");
    if (args != expected) std::cout << "  got: " << join(args) << std::endl;
    expect(!contains(args, "seccomp"), "no seccomp option is passed");
}

void test_halogen_command() {
    const auto args = ContainerManager::build_run_args(halogen_spec(), ContainerTool::Podman);
    for (const std::string option :
         {"--device /dev/dri", "--device /dev/kfd", "--group-add keep-groups", "--ipc=host",
          "--ulimit memlock=-1:-1", "--env HOME=/tmp", "--env HALOGEN_CTX=262144",
          "--env HALOGEN_KV_POOL_POSITIONS=524288", "--env HALOGEN_KV_SLOTS=4",
          "--env HALOGEN_PROMPT_CACHE=2", "--env HALOGEN_API_PORT=8001",
          "--env HIP_VISIBLE_DEVICES=0", "--env HALOGEN_TOKENIZER=/mnt/models/tokenizer",
          "-p 127.0.0.1:8001:8001", "--label ai.lemonade.model=Qwen3.8-Flash-Next-Halogen",
          "destination=/mnt/models/tokenizer,ro"}) {
        expect(contains(args, option), "Appendix B Halogen command has " + option);
    }
    expect(count_of(args, "--env") == 10, "Appendix B Halogen command sets ten variables");
    expect(!contains(args, "--cap-add"), "Halogen gets no capability back");
    expect(args.back() ==
               "ghcr.io/peonist-ai/halogen-flash-server@sha256:"
               "6e626c979d536ab1edb07898e278be6686afd353758ea268817457f801d687dd",
           "Halogen's command ends at the image, whose entrypoint starts the server");
}

void test_ctx_size_replaces_entry_value() {
    ContainerRunSpec spec = halogen_spec();
    spec.env.push_back({"HALOGEN_CTX", "8192"});
    const auto args = ContainerManager::build_run_args(spec, ContainerTool::Podman);
    expect(contains(args, "--env HALOGEN_CTX=8192") && !contains(args, "HALOGEN_CTX=262144"),
           "a ServerCommand variable replaces the entry's value for the same key");
    expect(contains(args, "--env HALOGEN_KV_SLOTS=4"), "the entry's other variables stay");
}

void test_extra_access() {
    ContainerRunSpec spec = nathanw_spec();
    spec.image.cap_add = {"SYS_PTRACE"};
    const auto args = ContainerManager::build_run_args(spec, ContainerTool::Podman);
    expect(contains(args, "--cap-add SYS_PTRACE"), "cap_add gives the capability back");
    expect(!contains(args, "--ipc=host") && !contains(args, "--ulimit"),
           "ipc and memlock only when the entry sets them");
}

void test_docker_differences() {
    const ContainerRunSpec spec = nathanw_spec();
    const auto args = ContainerManager::build_run_args(spec, ContainerTool::Docker);
    expect(!contains(args, "-p "), "Docker publishes no port from an --internal network");
    expect(!contains(args, "keep-groups"), "keep-groups is Podman's");
    bool numeric = true;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != "--group-add") continue;
        numeric = numeric && args[i + 1].find_first_not_of("0123456789") == std::string::npos;
    }
    expect(numeric, "Docker gets the host's numeric group ids");
    const std::string video = ContainerManager::host_group_gid("video");
    if (!video.empty()) {
        expect(contains(args, "--group-add " + video), "Docker gets the host's video gid");
    }
    expect(!ContainerManager::connects_on_loopback(spec, ContainerTool::Docker),
           "lemond reaches a Docker container at its address");
    expect(ContainerManager::connects_on_loopback(spec, ContainerTool::Podman),
           "lemond reaches a Podman container at 127.0.0.1");
}

void test_shared_network_namespace() {
    ContainerRunSpec spec = nathanw_spec();
    spec.network = "container:abc123";
    for (const auto tool : {ContainerTool::Podman, ContainerTool::Docker}) {
        const auto args = ContainerManager::build_run_args(spec, tool);
        expect(contains(args, "--network=container:abc123") && !contains(args, "-p "),
               "in the Docker image the server joins lemond's namespace with no -p");
        expect(ContainerManager::connects_on_loopback(spec, tool),
               "in the Docker image lemond reaches the server at 127.0.0.1");
    }
}

void test_volume_mounts() {
    ContainerRunSpec spec = nathanw_spec();
    ContainerMount volume;
    volume.host_path = "lemonade-cache";
    volume.container_path = "/mnt/models/model.gguf";
    volume.volume = true;
    volume.volume_subpath = "hub/blobs/deadbeef";
    spec.mounts = {volume};

    const auto docker_args = ContainerManager::build_run_args(spec, ContainerTool::Docker);
    expect(contains(docker_args, "--mount type=volume,src=lemonade-cache,destination="
                                 "/mnt/models/model.gguf,volume-subpath=hub/blobs/deadbeef,ro"),
           "Docker spells a volume subpath as volume-subpath");
    const auto podman_args = ContainerManager::build_run_args(spec, ContainerTool::Podman);
    expect(contains(podman_args, "--mount type=volume,src=lemonade-cache,destination="
                                 "/mnt/models/model.gguf,subpath=hub/blobs/deadbeef,ro"),
           "Podman spells a volume subpath as subpath");
}

void test_image_refs() {
    ContainerImage image;
    image.repository = "docker.io/kyuz0/amd-strix-halo-toolboxes";
    image.tag = "rocm-10.0";
    image.digest = "sha256:deadbeef";
    expect(image.pinned_ref() == "docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:deadbeef",
           "the image is <repository>@<digest>");
    expect(join(ContainerManager::build_pull_args(image)) ==
               "pull docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:deadbeef",
           "install pulls the digest");

    ContainerImage no_digest;
    no_digest.repository = "docker.io/kyuz0/amd-strix-halo-toolboxes";
    no_digest.tag = "v1";
    expect(!no_digest.valid(), "an image with no digest cannot run");

    expect(join(ContainerManager::build_stop_args("lemonade-x", 10)) == "stop --time 10 lemonade-x",
           "stop is by container name with a 10 second grace period");
}

void test_container_names() {
    expect(ContainerManager::container_name("llamacpp", "nathanw", "Qwen3-4B-GGUF") ==
               "lemonade-llamacpp-nathanw-Qwen3-4B-GGUF",
           "container name is lemonade-<recipe>-<backend>-<model>");
    expect(ContainerManager::container_name("we/ird", "a b", "user.M:1") ==
               "lemonade-we-ird-a-b-user.M-1",
           "unsafe characters are replaced");
    expect(std::string(ContainerManager::managed_label()) == "ai.lemonade",
           "the managed label is ai.lemonade");
}

void test_install_commands() {
    const auto command = [](const std::string& text) {
        return ContainerManager::podman_install_command(text);
    };
    expect(command("ID=ubuntu\nID_LIKE=debian\n") == "sudo apt install podman", "Ubuntu uses apt");
    expect(command("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n") == "sudo apt install podman",
           "Linux Mint matches debian through ID_LIKE");
    expect(command("ID=debian\n") == "sudo apt install podman", "Debian uses apt");
    expect(command("ID=fedora\n") == "sudo dnf install podman", "Fedora uses dnf");
    expect(command("ID=\"rhel\"\nID_LIKE=\"fedora\"\n") == "sudo dnf install podman",
           "RHEL matches fedora through ID_LIKE");
    expect(command("ID=\"rocky\"\nID_LIKE=\"rhel centos fedora\"\n") == "sudo dnf install podman",
           "Rocky Linux matches fedora through ID_LIKE");
    expect(command("ID=arch\n") == "sudo pacman -S podman", "Arch uses pacman");
    expect(command("ID=cachyos\nID_LIKE=arch\n") == "sudo pacman -S podman",
           "CachyOS matches arch through ID_LIKE");
    expect(command("ID=nixos\n") == "Install Podman with the host's package manager",
           "an unmatched distribution gets the generic text");
    expect(command("") == "Install Podman with the host's package manager",
           "a missing os-release gets the generic text");
}

void test_parsers() {
    const std::string images_output =
        "kyuz0/amd-strix-halo-toolboxes@sha256:aaa\nother/image@sha256:bbb\n";
    const auto matched = ContainerManager::parse_repo_digests(
        images_output, "docker.io/kyuz0/amd-strix-halo-toolboxes");
    expect(matched.size() == 1 && matched[0] == "sha256:aaa",
           "digest matched with the docker.io prefix stripped");
    expect(ContainerManager::parse_repo_digests("a/b@<none>", "a/b").empty(),
           "a tag-only image reports no digest");
}

void test_gpu_selection() {
    expect(ContainerManager::gfx_name_from_target_version(110501) == "gfx1151",
           "Strix Halo target version names gfx1151");
    expect(ContainerManager::gfx_name_from_target_version(90010) == "gfx90a",
           "stepping renders as a hex digit");
    const std::vector<int> topology = {0, 110000, 110501};
    expect(ContainerManager::pick_gpu_index(topology, "gfx1151") == "1",
           "the GPU index skips CPU nodes and matches the image's arch");
    expect(ContainerManager::pick_gpu_index(topology, "gfx1201").empty(),
           "an arch that is not present selects nothing");
}

void test_self_mount_parsing() {
    const std::string mounts =
        R"([{"Type":"volume","Name":"lemonade-cache","Source":"/var/lib/docker/volumes/lemonade-cache/_data","Destination":"/opt/lemonade/.cache/huggingface","RW":true},)"
        R"({"Type":"bind","Source":"/srv/models","Destination":"/models","RW":false}])";
    const auto parsed = ContainerManager::parse_self_mounts(mounts);
    expect(parsed.size() == 2, "every mount is parsed");
    expect(parsed[0].volume && parsed[0].host_path == "lemonade-cache",
           "a named volume is addressed by name");
    expect(!parsed[1].volume && parsed[1].host_path == "/srv/models" && parsed[1].read_only,
           "a bind mount keeps its host source");
}

#ifndef _WIN32

void test_tool_choice() {
    {
        FakePath path({"podman", "docker"});
        FakeTool fake;
        ContainerManager manager(fake.runner(), HostTransport::Native);
        const auto info = manager.info();
        expect(info && info->tool == ContainerTool::Podman,
               "Podman is used when both tools are installed");
    }
    {
        FakePath path({"docker"});
        FakeTool fake;
        ContainerManager manager(fake.runner(), HostTransport::Native);
        const auto info = manager.info();
        expect(info && info->tool == ContainerTool::Docker,
               "Docker is used when Podman is not installed");
    }
    {
        FakePath path({});
        FakeTool fake;
        ContainerManager manager(fake.runner(), HostTransport::Native);
        expect(!manager.info(), "no tool when neither is installed");
    }
}

void test_command_prefixes() {
    FakePath path({"podman"});
    FakeTool fake;

    ::unsetenv("CONTAINER_HOST");
    ContainerManager native(fake.runner(), HostTransport::Native);
    auto [exe, prefix] = native.invocation(podman_info());
    expect(exe == "/usr/bin/podman" && prefix.empty(), "a user's lemond runs podman as is");

    ::setenv("CONTAINER_HOST", "unix:///run/lemonade-podman.sock", 1);
    std::tie(exe, prefix) = native.invocation(podman_info());
    expect(prefix == std::vector<std::string>{"--remote"},
           "lemond.service runs podman --remote through CONTAINER_HOST");
    ::unsetenv("CONTAINER_HOST");

    for (const auto transport : {HostTransport::Container, HostTransport::Snap}) {
        ContainerManager sandboxed(fake.runner(), transport);
        std::tie(exe, prefix) = sandboxed.invocation(podman_info());
        expect(prefix == std::vector<std::string>{"--remote"},
               "the bundled podman in the Docker image and the snap runs only with --remote");
    }

    ContainerManager toolbox(fake.runner(), HostTransport::Toolbox);
    std::tie(exe, prefix) = toolbox.invocation(podman_info());
    expect(fs::path(exe).filename() == "flatpak-spawn" &&
               prefix == std::vector<std::string>{"--host", "podman"},
           "a toolbox runs flatpak-spawn --host podman");
}

void test_run_and_stop_sequence() {
    FakePath path({"podman"});
    FakeTool fake;
    ContainerManager manager(fake.runner(), HostTransport::Native);

    const auto command = manager.run_command(nathanw_spec());
    expect(fs::path(command.front()).filename() == "podman" && command[1] == "run",
           "the run command starts with the tool, then run");

    manager.ensure_isolated_network("lemonade-x");
    fake.answers["network inspect"] = {1, "no such network"};
    manager.ensure_isolated_network("lemonade-y");
    const auto creates = fake.calls_starting_with("network");
    expect(!creates.empty() &&
               creates.back() == "network create --internal --label ai.lemonade lemonade-y",
           "the private network is created --internal with the managed label");
    fake.answers.erase("network inspect");

    fake.calls.clear();
    manager.stop("lemonade-x");
    std::vector<std::string> sequence;
    for (const auto& [exe, args] : fake.calls) sequence.push_back(join(args));
    expect(sequence.size() == 3 && sequence[0] == "stop --time 10 lemonade-x" &&
               sequence[1] == "rm -f lemonade-x" && sequence[2] == "network rm lemonade-x",
           "stop runs stop --time 10, then removes the container and its network");
}

void test_sweep() {
    FakePath path({"podman"});
    FakeTool fake;
    fake.answers["ps"] = {0, "lemonade-a\nlemonade-b\n"};
    ContainerManager manager(fake.runner(), HostTransport::Native);
    expect(manager.sweep_managed_containers() == 2, "every labelled container is removed");
    const auto listed = fake.calls_starting_with("ps");
    expect(listed.size() == 1 && listed[0] == "ps --all --filter label=ai.lemonade --format "
                                              "{{.Names}}",
           "the sweep lists stopped containers too, by the managed label");
}

void test_native_readiness() {
    {
        FakePath path({});
        FakeTool fake;
        ContainerManager manager(fake.runner(), HostTransport::Native);
        const auto result = manager.check_readiness({"/dev/dri"});
        expect(!result.ok && result.message == "podman is not on PATH" && !result.tool_usable,
               "with neither tool, the first row of the Podman table fails");
        expect(!result.action.empty(), "the fix is an install command");
    }
    {
        FakePath path({"docker"});
        FakeTool fake;
        fake.answers["version"] = {1, "permission denied"};
        ContainerManager manager(fake.runner(), HostTransport::Native);
        const auto result = manager.check_readiness({"/dev/dri"});
        expect(!result.ok && result.message == "The Docker daemon refuses the user's account" &&
                   result.action == "sudo usermod -aG docker $USER\nLog out and back in",
               "Docker that refuses the user names the docker group fix");
    }
    {
        FakePath path({"docker"});
        FakeTool fake;
        ContainerManager manager(fake.runner(), HostTransport::Native);
        expect(manager.check_readiness({"/dev/dri"}).ok,
               "Docker opens the device nodes itself, so no group check applies");
    }
    {
        FakePath path({"podman"});
        FakeTool fake;
        ContainerManager manager(fake.runner(), HostTransport::Native);
        const auto result = manager.check_readiness({"/dev/dri"});
        expect(result.ok ||
                   (result.message == "The user's account is not in both video and render" &&
                    result.action == "sudo usermod -aG video,render $USER\nLog out and back in" &&
                    result.tool_usable),
               "rootless Podman checks the account's video and render membership");
    }
}

void test_snap_readiness() {
    FakePath path({"podman", "docker"});
    {
        FakeTool fake;
        fake.answers["snapctl podman"] = {1, ""};
        fake.answers["snapctl docker"] = {1, ""};
        ContainerManager manager(fake.runner(), HostTransport::Snap);
        const auto result = manager.check_readiness({"/dev/dri"});
        expect(!result.ok &&
                   result.message == "Neither the podman plug nor the docker plug is connected" &&
                   contains({result.action}, "sudo systemctl enable --now podman.socket\n"
                                             "sudo snap connect lemonade-server:podman :podman"),
               "the snap with no plug connected names the Podman setup");
    }
    {
        FakeTool fake;
        fake.answers["snapctl podman"] = {0, ""};
        fake.answers["version"] = {1, "connection refused"};
        ContainerManager manager(fake.runner(), HostTransport::Snap);
        const auto result = manager.check_readiness({"/dev/dri"});
        expect(!result.ok && result.message == "/run/podman/podman.sock does not answer" &&
                   result.action == "sudo systemctl enable --now podman.socket",
               "the snap's podman plug with no service names podman.socket");
    }
    {
        FakeTool fake;
        fake.answers["snapctl podman"] = {1, ""};
        fake.answers["snapctl docker"] = {0, ""};
        fake.answers["version"] = {1, "connection refused"};
        ContainerManager manager(fake.runner(), HostTransport::Snap);
        const auto info = manager.info();
        const auto result = manager.check_readiness({"/dev/dri"});
        expect(info && info->tool == ContainerTool::Docker,
               "the snap uses Docker when only its docker plug is connected");
        expect(!result.ok && result.message == "The docker snap's daemon does not answer" &&
                   result.action == "sudo snap start docker",
               "a stopped docker snap names snap start");
    }
}

void test_toolbox_readiness() {
    FakePath path({"flatpak-spawn"});
    FakeTool fake;
    fake.answers["sh"] = {1, ""};
    ContainerManager manager(fake.runner(), HostTransport::Toolbox);
    const auto result = manager.check_readiness({"/dev/dri"});
    expect(!result.ok && result.message == "podman is not on the host's PATH",
           "a toolbox with no tool on the host names the Podman install");
}

void test_container_readiness() {
    const bool podman_socket = fs::exists("/run/podman/podman.sock");
    const bool docker_socket = fs::exists("/var/run/docker.sock");
    FakePath path({"podman", "docker"});
    FakeTool fake;
    fake.answers["inspect"] = {1, "no such container"};
    ContainerManager manager(fake.runner(), HostTransport::Container);
    const auto result = manager.check_readiness({"/dev/dri"});
    if (!podman_socket && !docker_socket) {
        expect(!result.ok &&
                   result.message == "No socket is mounted at /run/podman/podman.sock or "
                                     "/var/run/docker.sock" &&
                   result.action ==
                       "https://lemonade-server.ai/docs/guide/install/docker/#container-backends",
               "the Docker image with no socket links to the Docker install guide");
    } else {
        expect(!result.ok &&
                   result.message == "lemond cannot inspect its own container through the socket",
               "the Docker image that cannot inspect itself says so");
    }
}

#endif

}  // namespace

int main() {
    test_spec_example_command();
    test_halogen_command();
    test_ctx_size_replaces_entry_value();
    test_extra_access();
    test_docker_differences();
    test_shared_network_namespace();
    test_volume_mounts();
    test_image_refs();
    test_container_names();
    test_install_commands();
    test_parsers();
    test_gpu_selection();
    test_self_mount_parsing();
#ifndef _WIN32
    test_tool_choice();
    test_command_prefixes();
    test_run_and_stop_sequence();
    test_sweep();
    test_native_readiness();
    test_snap_readiness();
    test_toolbox_readiness();
    test_container_readiness();
#endif

    if (failures == 0) {
        std::cout << "All container manager tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " container manager test(s) failed" << std::endl;
    return 1;
}
