// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/fastflowlm/fastflowlm_models.h"
#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/backends/whispercpp/whispercpp_server.h"
#include "lemon/config_file.h"
#include "lemon/model_manager.h"
#include "lemon/models/registry_id.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

// What each backend says its models need fetched. The ingest path asks this
// question instead of naming backends, so the three answers are pinned here.
namespace download_policy {

// What each backend says its models need fetched. The ingest path asks this
// question instead of naming backends, so the three answers are pinned here.


using lemon::ModelInfo;
using lemon::backends::BackendOpsContext;
using lemon::backends::DownloadPolicy;
using lemon::backends::ops_for;

namespace {

int g_failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

DownloadPolicy policy_for(const std::string& recipe) {
    ModelInfo info;
    info.recipe = recipe;
    return ops_for(recipe)->plan_download(info, BackendOpsContext{});
}

}  // namespace

int run() {
    check("a registry model is fetched into the models directory",
          policy_for("llamacpp") == DownloadPolicy::Repository);
    check("flm pulls its own weights", policy_for("flm") == DownloadPolicy::SelfManaged);
    check("cloud has no bytes to fetch", policy_for("cloud") == DownloadPolicy::None);
    // A recipe the registry does not know falls the same way as an ordinary local
    // model, so an unregistered recipe is never silently skipped.
    check("an unknown recipe asks a repository",
          policy_for("no-such-backend") == DownloadPolicy::Repository);

    if (g_failures == 0) {
        std::printf("All download-policy tests passed.\n");
    } else {
        std::printf("%d download-policy test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace download_policy


namespace engine_protocols {

using lemon::backends::BackendOps;
using lemon::models::download_protocols::hf_hub;
using lemon::models::download_protocols::modelscope_api;
using lemon::models::RegistryId;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

static void test_an_engine_that_cannot_resolve_ids_gets_local_paths() {
    const BackendOps& ops = *lemon::backends::whispercpp::ops();
    check("a backend is handed local paths unless it says otherwise",
          !ops.resolves_download_protocol(hf_hub));
    check("and not even for a protocol it has never heard of",
          !ops.resolves_download_protocol("carrier-pigeon-ftp"));
}

static void test_an_engine_names_the_protocols_it_resolves() {
    const BackendOps& ops = *lemon::backends::vllm::ops();
    check("vLLM resolves ids on a plain hub host", ops.resolves_download_protocol(hf_hub));
    check("vLLM does not resolve ids on a host serving files through its API",
          !ops.resolves_download_protocol(modelscope_api));
    check("vLLM does not resolve ids for a protocol it cannot name",
          !ops.resolves_download_protocol(""));
}

static void test_the_answer_is_a_capability_not_a_host_name() {
    const BackendOps& ops = *lemon::backends::vllm::ops();
    check("naming a host is not a protocol, so it resolves nothing",
          !ops.resolves_download_protocol(RegistryId("modelscope").str()));
    check("neither is a host's short spelling",
          !ops.resolves_download_protocol(RegistryId("ms").str()));
    check("a hub's own name is still not its protocol",
          !ops.resolves_download_protocol(RegistryId("huggingface").str()));
}

}  // namespace

int run() {
    test_an_engine_that_cannot_resolve_ids_gets_local_paths();
    test_an_engine_names_the_protocols_it_resolves();
    test_the_answer_is_a_capability_not_a_host_name();

    if (g_failures != 0) {
        std::printf("FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("All engine protocol capability checks passed\n");
    return 0;
}

}  // namespace engine_protocols


namespace flm_pull_args {

using lemon::models::detail::add_descriptor;
using lemon::models::RegistryDescriptor;
using lemon::models::RegistryId;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

bool has(const std::vector<std::string>& args, const std::string& value) {
    for (const auto& arg : args) {
        if (arg == value) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> pull(const std::string& source, bool do_not_upgrade = true) {
    return lemon::backends::fastflowlm::flm_pull_arguments("org/repo", do_not_upgrade,
                                                           RegistryId(source));
}

bool throws(const std::string& source) {
    try {
        pull(source);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

static void test_the_flag_follows_the_protocol() {
    const std::vector<std::string> api_host = pull("modelscope");
    check("a host served through its own API gets flm's flag for that",
          has(api_host, "--modelscope") && has(api_host, "1"));

    const std::vector<std::string> hub = pull("huggingface");
    check("a plain hub host gets no flag", !has(hub, "--modelscope"));
    check("the pull still names the checkpoint",
          hub.size() == 2 && hub[0] == "pull" && hub[1] == "org/repo");
    check("a refresh still forces", has(pull("huggingface", false), "--force"));
}

static void test_an_unnamed_host_is_the_default_host() {
    check("an entry naming no host pulls from the default, with no flag",
          !has(pull(""), "--modelscope"));
}

static void test_an_unrecognized_host_stops_the_pull() {
    check("an unrecognized host is refused instead of pulling from the default",
          throws("modelplace"));
}

static void test_a_protocol_flm_cannot_express_stops_the_pull() {
    RegistryDescriptor pigeon;
    pigeon.id = RegistryId("carrier-pigeon");
    pigeon.aliases = {"carrier-pigeon"};
    pigeon.display_name = "Carrier Pigeon";
    pigeon.download_protocol = "carrier-pigeon-ftp";
    add_descriptor(pigeon);

    check("a protocol with no flag mapping stops the pull rather than guessing a host",
          throws("carrier-pigeon"));
}

}  // namespace

int run() {
    test_the_flag_follows_the_protocol();
    test_an_unnamed_host_is_the_default_host();
    test_an_unrecognized_host_stops_the_pull();
    test_a_protocol_flm_cannot_express_stops_the_pull();

    if (g_failures != 0) {
        std::printf("FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("All flm pull-argument checks passed\n");
    return 0;
}

}  // namespace flm_pull_args

namespace backend_uninstall {
namespace {

namespace fs = std::filesystem;
using lemon::json;

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(std::string name)
        : name_(std::move(name)), had_value_(std::getenv(name_.c_str()) != nullptr),
          old_value_(lemon::utils::get_environment_variable_utf8(name_)) {
        lemon::utils::unset_environment_variable_utf8(name_);
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            lemon::utils::set_environment_variable_utf8(name_, old_value_);
        } else {
            lemon::utils::unset_environment_variable_utf8(name_);
        }
    }

    void set(const fs::path& value) const {
        lemon::utils::set_environment_variable_utf8(name_, lemon::utils::path_to_utf8(value));
    }

private:
    std::string name_;
    bool had_value_;
    std::string old_value_;
};

class TestSystemInfo final : public lemon::SystemInfo {
public:
    lemon::CPUInfo get_cpu_device() override { return {}; }
    lemon::GPUInfo get_amd_igpu_device() override { return {}; }
    std::vector<lemon::GPUInfo> get_amd_dgpu_devices() override { return {}; }
    std::vector<lemon::GPUInfo> get_nvidia_gpu_devices() override { return {}; }
    lemon::NPUInfo get_npu_device() override { return {}; }
};

json build_recipes(const json& config_json, std::string* flm_path = nullptr) {
    lemon::RuntimeConfig config(config_json);
    auto* previous = lemon::RuntimeConfig::global();
    lemon::RuntimeConfig::set_global(&config);
    try {
        TestSystemInfo system_info;
        json result = system_info.build_recipes_info({
            {"cpu", {{"name", "Test CPU"}, {"family", "x86_64"}}},
        });
        if (flm_path) {
            *flm_path = lemon::backends::fastflowlm::find_flm_executable();
        }
        lemon::RuntimeConfig::set_global(previous);
        return result;
    } catch (...) {
        lemon::RuntimeConfig::set_global(previous);
        throw;
    }
}

void write_binary(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << "stub";
    file.close();
    if (!file) throw std::runtime_error("Could not write backend fixture");
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
#endif
}

}  // namespace

int run() {
    int failures = 0;
    const auto check = [&failures](bool ok, const char* message) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };
    const auto can_uninstall = [](const json& recipes, const char* recipe, const char* backend) {
        return recipes.at(recipe).at("backends").at(backend).at("can_uninstall").get<bool>();
    };

    const fs::path scratch = fs::path(lemon::utils::get_cache_dir()) / "backend-uninstall";
    const fs::path empty_path = scratch / "empty";
    const fs::path system_path = scratch / "system";
    fs::create_directories(empty_path);
#ifdef _WIN32
    const std::string binary_name = "flm.exe";
#else
    const std::string binary_name = "flm";
#endif
    write_binary(system_path / binary_name);
    const fs::path managed_flm = fs::path(lemon::utils::get_downloaded_bin_dir())
        / "flm" / "npu" / binary_name;
    write_binary(managed_flm);

    ScopedEnvVar path_env("PATH");
    ScopedEnvVar flm_bin_env("LEMONADE_FLM_NPU_BIN");
    ScopedEnvVar cpu_bin_env("LEMONADE_LLAMACPP_CPU_BIN");
    path_env.set(empty_path);
    json config = lemon::ConfigFile::base_defaults();
    config["flm"]["prefer_system"] = false;
    std::string flm_path;
    const json managed = build_recipes(config, &flm_path);
    check(!can_uninstall(managed, "llamacpp", "system"), "System backend cannot be uninstalled");
    check(can_uninstall(managed, "llamacpp", "cpu"), "Managed llama.cpp can be uninstalled");
    check(can_uninstall(managed, "flm", "npu"), "Managed FLM can be uninstalled");
    check(fs::equivalent(flm_path, managed_flm), "FLM resolves to managed binary");

    config["flm"]["prefer_system"] = true;
    const json fallback = build_recipes(config, &flm_path);
    check(can_uninstall(fallback, "flm", "npu"), "Missing system FLM keeps managed uninstall");
    check(fs::equivalent(flm_path, managed_flm), "Missing system FLM resolves to managed binary");

    path_env.set(system_path);
    const json system = build_recipes(config, &flm_path);
    check(!can_uninstall(system, "flm", "npu"), "System FLM cannot be uninstalled");
    check(flm_path == lemon::utils::find_executable_in_path(binary_name), "FLM resolves to system binary");
    config["flm"]["prefer_system"] = false;
    check(can_uninstall(build_recipes(config), "flm", "npu"), "Disabled system FLM keeps managed uninstall");

    config["flm"]["npu_bin"] = lemon::utils::path_to_utf8(system_path);
    check(!can_uninstall(build_recipes(config), "flm", "npu"), "External FLM cannot be uninstalled");
    config["llamacpp"]["cpu_bin"] = lemon::utils::path_to_utf8(system_path);
    check(!can_uninstall(build_recipes(config), "llamacpp", "cpu"), "External llama.cpp cannot be uninstalled");
    config["llamacpp"]["cpu_bin"] = lemon::utils::path_to_utf8(scratch / "missing");
    check(!can_uninstall(build_recipes(config), "llamacpp", "cpu"), "Missing external path keeps recipe metadata");
    config["llamacpp"]["cpu_bin"] = "latest";
    check(can_uninstall(build_recipes(config), "llamacpp", "cpu"), "Latest version pin keeps managed uninstall");
    config["llamacpp"]["cpu_bin"] = "b1234";
    check(can_uninstall(build_recipes(config), "llamacpp", "cpu"), "Version tag keeps managed uninstall");

    config = lemon::ConfigFile::base_defaults();
    config["flm"]["prefer_system"] = false;
    flm_bin_env.set(system_path);
    check(!can_uninstall(build_recipes(config), "flm", "npu"), "Environment FLM override cannot be uninstalled");
    return failures == 0 ? 0 : 1;
}

}  // namespace backend_uninstall

int main() {
    int failures = 0;
    std::printf("\n=== download policy ===\n");
    failures |= download_policy::run();
    std::printf("\n=== engine protocols ===\n");
    failures |= engine_protocols::run();
    std::printf("\n=== flm pull args ===\n");
    failures |= flm_pull_args::run();
    std::printf("\n=== backend uninstall ===\n");
    failures |= backend_uninstall::run();
    return failures;
}
