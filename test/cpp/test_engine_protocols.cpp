#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/backends/whispercpp/whispercpp_server.h"
#include "lemon/models/registry_id.h"

#include <string>

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

int main() {
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
