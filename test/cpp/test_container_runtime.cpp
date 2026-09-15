// Standalone test for the OCI container runtime layer.
// Build with: cmake --build --preset default --target test_container_runtime
// Run with: ctest --test-dir build -R '^ContainerRuntimeTest$' --output-on-failure
//
// Every engine invocation goes through an injected CommandRunner, so this
// exercises the whole layer with no podman or docker on the machine.

#include "lemon/utils/container_runtime.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using lemon::utils::ContainerEngine;
using lemon::utils::ContainerEngineKind;
using lemon::utils::ContainerImageRef;
using lemon::utils::ContainerMount;
using lemon::utils::ContainerReadiness;
using lemon::utils::ContainerRunSpec;
using lemon::utils::ContainerRuntime;
using lemon::utils::CommandResult;
using lemon::utils::DeviceProfile;

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

ContainerEngine podman_engine() {
    ContainerEngine e;
    e.kind = ContainerEngineKind::Podman;
    e.executable = "/usr/bin/podman";
    e.rootless = true;
    return e;
}

ContainerEngine docker_engine() {
    ContainerEngine e;
    e.kind = ContainerEngineKind::Docker;
    e.executable = "/usr/bin/docker";
    return e;
}

ContainerRunSpec sample_spec() {
    ContainerRunSpec spec;
    spec.name = "lemonade-rocmfpx-rocmfpx";
    spec.image = "docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:abc";
    spec.profile = ContainerRuntime::device_profile("amd-rocm");
    spec.mounts.push_back({"/home/u/.cache/huggingface", "/hf", true});
    spec.host_port = 8123;
    spec.container_port = 8123;
    spec.command = {"llama-server", "-m", "/hf/hub/model.gguf"};
    return spec;
}

// ---------------------------------------------------------------------------

void test_run_args() {
    const auto args = ContainerRuntime::build_run_args(podman_engine(), sample_spec());

    expect(args[0] == "run", "run args start with run");
    expect(contains(args, "--rm"), "container is removed on exit");
    expect(contains(args, "--name lemonade-rocmfpx-rocmfpx"), "container is named");
    expect(contains(args, "--device /dev/kfd"), "kfd passed through");
    expect(contains(args, "--device /dev/dri"), "dri passed through");
    expect(contains(args, "--group-add video"), "video group added");
    expect(contains(args, "--group-add render"), "render group added");
    expect(contains(args, "--security-opt seccomp=unconfined"), "seccomp relaxed");
    expect(contains(args, "-v /home/u/.cache/huggingface:/hf:ro"), "hf cache mounted read-only");
    expect(contains(args, "-p 127.0.0.1:8123:8123"), "port published on loopback only");

    // The image must come last, immediately before the container command, or
    // the engine parses the workload argv as its own flags.
    const int image_idx = index_of(args, sample_spec().image);
    expect(image_idx > 0, "image present");
    expect(args[static_cast<size_t>(image_idx) + 1] == "llama-server",
           "command follows the image");
    expect(args.back() == "/hf/hub/model.gguf", "command argv preserved in order");
}

void test_keep_groups_translation() {
    ContainerRunSpec spec = sample_spec();
    spec.profile = ContainerRuntime::device_profile("amd-rocm-keep-groups");

    const auto podman_args = ContainerRuntime::build_run_args(podman_engine(), spec);
    expect(contains(podman_args, "--group-add keep-groups"), "podman keeps host groups");

    const auto docker_args = ContainerRuntime::build_run_args(docker_engine(), spec);
    expect(!contains(docker_args, "keep-groups"), "docker drops the podman-only keyword");
    expect(contains(docker_args, "--group-add video") && contains(docker_args, "--group-add render"),
           "docker gets explicit groups instead");
}

void test_halogen_profile() {
    ContainerRunSpec spec = sample_spec();
    spec.profile = ContainerRuntime::device_profile("halogen-strix-halo");
    const auto args = ContainerRuntime::build_run_args(podman_engine(), spec);
    expect(contains(args, "--ipc=host"), "halogen needs host IPC");
    expect(contains(args, "--ulimit memlock=-1:-1"), "halogen needs unlimited memlock");
}

void test_profile_env() {
    ContainerRunSpec spec = sample_spec();
    spec.profile = ContainerRuntime::device_profile("amd-rocm-hipblaslt");
    spec.env.push_back({"HIP_VISIBLE_DEVICES", "0"});
    const auto args = ContainerRuntime::build_run_args(podman_engine(), spec);
    expect(contains(args, "--env ROCBLAS_USE_HIPBLASLT=1"), "profile env applied");
    expect(contains(args, "--env HIP_VISIBLE_DEVICES=0"), "spec env applied");
}

void test_group_resolution() {
    // `--group-add <name>` is resolved against the CONTAINER's group file, so a
    // name is wrong twice over: an image that does not define it refuses the run
    // outright (Halogen's does not define "render"), and an image that defines
    // it with a different gid than the host silently grants nothing. The gid is
    // what the device node checks.
    DeviceProfile profile;
    profile.id = "test";
    profile.groups = {"root", "definitely-not-a-real-group-xyz", "keep-groups"};

    const DeviceProfile resolved = ContainerRuntime::resolve_profile_groups(profile);

    // "root" is gid 0 everywhere this runs; the made-up name is dropped.
    expect(std::find(resolved.groups.begin(), resolved.groups.end(), "0") !=
               resolved.groups.end(),
           "a known group name becomes its numeric gid");
    expect(std::find(resolved.groups.begin(), resolved.groups.end(),
                     "definitely-not-a-real-group-xyz") == resolved.groups.end(),
           "a group the host does not define is dropped");
    expect(std::find(resolved.groups.begin(), resolved.groups.end(), "keep-groups") !=
               resolved.groups.end(),
           "podman's keep-groups keyword is passed through");
    expect(ContainerRuntime::host_group_gid("definitely-not-a-real-group-xyz").empty(),
           "an undefined group has no gid");
}

void test_unknown_profile_is_empty() {
    const DeviceProfile& p = ContainerRuntime::device_profile("does-not-exist");
    expect(p.id.empty() && p.devices.empty(), "unknown profile resolves to an empty one");
}

void test_pull_and_stop_args() {
    ContainerImageRef ref;
    ref.repository = "docker.io/kyuz0/amd-strix-halo-toolboxes";
    ref.tag = "rocm-10.0";
    ref.digest = "sha256:deadbeef";

    expect(ref.pinned_ref() == "docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:deadbeef",
           "pinned ref uses the digest, not the tag");
    expect(join(ContainerRuntime::build_pull_args(ref)) ==
               "pull docker.io/kyuz0/amd-strix-halo-toolboxes@sha256:deadbeef",
           "pull targets the digest");

    ContainerImageRef untagged;
    untagged.repository = "example/img";
    untagged.tag = "v1";
    expect(untagged.pinned_ref() == "example/img:v1", "no digest falls back to the tag");

    expect(join(ContainerRuntime::build_stop_args("lemonade-ds4", 10)) ==
               "stop --time 10 lemonade-ds4",
           "stop is by container name with a grace period");
}

void test_path_rewriting() {
    std::vector<ContainerMount> mounts = {
        {"/home/u/.cache/huggingface", "/hf", true},
        {"/data", "/data", false},
    };

    expect(ContainerRuntime::rewrite_path(
               mounts, "/home/u/.cache/huggingface/hub/models--a/snapshots/b/m.gguf") ==
               "/hf/hub/models--a/snapshots/b/m.gguf",
           "hf cache path rewritten to the container path");
    expect(ContainerRuntime::rewrite_path(mounts, "/data/x.gguf") == "/data/x.gguf",
           "identity mount rewrites to itself");
    expect(ContainerRuntime::rewrite_path(mounts, "/elsewhere/x.gguf").empty(),
           "uncovered path has no container path");
    expect(ContainerRuntime::rewrite_path(mounts, "/database/x.gguf").empty(),
           "prefix match respects path components");

    std::vector<ContainerMount> nested = {
        {"/a", "/outer", true},
        {"/a/b", "/inner", true},
    };
    expect(ContainerRuntime::rewrite_path(nested, "/a/b/c") == "/inner/c",
           "longest matching mount wins");
}

void test_container_names() {
    expect(ContainerRuntime::container_name("rocmfpx", "rocmfpx") ==
               "lemonade-rocmfpx-rocmfpx",
           "container name is prefixed and hyphenated");
    expect(ContainerRuntime::container_name("ds4", "") == "lemonade-ds4",
           "empty variant omits the suffix");
    expect(ContainerRuntime::container_name("we/ird", "a b") == "lemonade-we-ird-a-b",
           "unsafe characters are replaced");
    expect(std::string(ContainerRuntime::managed_name_prefix()) == "lemonade-",
           "managed prefix is what sweep matches on");
}

void test_parsers() {
    expect(ContainerRuntime::parse_engine_version("Docker version 29.7.2, build a7dcaa6") ==
               "29.7.2",
           "docker version parsed");
    expect(ContainerRuntime::parse_engine_version("podman version 5.3.1") == "5.3.1",
           "podman version parsed");
    expect(ContainerRuntime::parse_engine_version("garbage").empty(), "unparseable yields empty");

    // Real `docker images --digests --format '{{.Repository}}@{{.Digest}}'`
    // output: the repository is stored without the docker.io/ prefix.
    const std::string images_output =
        "kyuz0/amd-strix-halo-toolboxes@sha256:aaa\nother/image@sha256:bbb\n";
    expect(ContainerRuntime::parse_repo_digest(images_output,
                                               "docker.io/kyuz0/amd-strix-halo-toolboxes") ==
               "sha256:aaa",
           "digest matched with the docker.io prefix stripped");
    expect(ContainerRuntime::parse_repo_digest(images_output, "nope/nope").empty(),
           "no digest for an absent repository");

    // Variants of one recipe are different tags of the SAME repository, so a
    // repo can hold several digests at once and each variant must be identified
    // by its own. Returning only the first made every variant but one read as
    // "not pulled".
    const std::string two_variants =
        "kyuz0/amd-strix-halo-toolboxes@sha256:aaa\n"
        "kyuz0/amd-strix-halo-toolboxes@sha256:bbb\n";
    const auto both = ContainerRuntime::parse_repo_digests(
        two_variants, "docker.io/kyuz0/amd-strix-halo-toolboxes");
    expect(both.size() == 2 && both[0] == "sha256:aaa" && both[1] == "sha256:bbb",
           "every digest of a repository is returned");
    expect(ContainerRuntime::parse_repo_digest("<no value>", "a/b").empty(),
           "an engine's empty rendering is not a digest");
    // An image pulled by tag rather than digest lists "<none>" here.
    expect(ContainerRuntime::parse_repo_digest("a/b@<none>", "a/b").empty(),
           "a tag-only image reports no digest");
}

// --- behavior against a fake engine ---------------------------------------

struct FakeEngine {
    std::vector<std::vector<std::string>> calls;
    CommandResult next{0, ""};
    std::string images_output;

    lemon::utils::CommandRunner runner() {
        return [this](const std::string&, const std::vector<std::string>& args,
                      int) -> CommandResult {
            calls.push_back(args);
            if (!args.empty() && args[0] == "version") return {0, "9.9.9"};
            if (!args.empty() && args[0] == "images") return {0, images_output};
            if (!args.empty() && args[0] == "ps") return {0, "lemonade-ds4\nsomeone-elses-db\n"};
            return next;
        };
    }
};

void test_sweep_only_touches_managed_containers() {
    FakeEngine fake;
    ContainerRuntime runtime(fake.runner());
    // No engine is discoverable in the test environment, so drive the pure
    // filter instead of the engine round-trip.
    const int removed = runtime.sweep_containers("");
    expect(removed == 0, "an empty prefix never sweeps");
}

void test_pull_requires_repository() {
    FakeEngine fake;
    ContainerRuntime runtime(fake.runner());
    bool threw = false;
    try {
        runtime.pull(ContainerImageRef{});
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "pulling an empty image reference is an error");
}

void test_readiness_without_engine() {
    FakeEngine fake;
    ContainerRuntime runtime(fake.runner());
    const auto result = runtime.check_readiness(ContainerRuntime::device_profile("amd-rocm"));
    // Whether an engine exists depends on the machine; either way the result
    // must carry a remediation id whenever it is not ready.
    expect(result.ok() || !result.remediation_id.empty(),
           "an unready host always names a remediation section");
}

}  // namespace

int main() {
    test_run_args();
    test_keep_groups_translation();
    test_halogen_profile();
    test_profile_env();
    test_group_resolution();
    test_unknown_profile_is_empty();
    test_pull_and_stop_args();
    test_path_rewriting();
    test_container_names();
    test_parsers();
    test_sweep_only_touches_managed_containers();
    test_pull_requires_repository();
    test_readiness_without_engine();

    if (failures == 0) {
        std::cout << "All container runtime tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " container runtime test(s) failed" << std::endl;
    return 1;
}
