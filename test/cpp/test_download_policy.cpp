// What each backend says its models need fetched. The ingest path asks this
// question instead of naming backends, so the three answers are pinned here.
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/model_manager.h"

#include <cstdio>
#include <string>

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

int main() {
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
