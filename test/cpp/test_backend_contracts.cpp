// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/model_manager.h"
#include <cstdio>
#include <string>
#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/backends/whispercpp/whispercpp_server.h"
#include "lemon/models/registry_id.h"
#include "lemon/backends/fastflowlm/fastflowlm_models.h"
#include <stdexcept>
#include <vector>

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

int main() {
    int failures = 0;
    std::printf("\n=== download policy ===\n");
    failures |= download_policy::run();
    std::printf("\n=== engine protocols ===\n");
    failures |= engine_protocols::run();
    std::printf("\n=== flm pull args ===\n");
    failures |= flm_pull_args::run();
    return failures;
}
