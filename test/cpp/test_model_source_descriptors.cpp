#include <lemon/model_manager.h>
#include <lemon/models/registry_types.h>
#include <lemon/models/model_sources.h>
#include <lemon/models/registry_id.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/path_utils.h>

#include "support/test_state_isolation.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using lemon::json;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::models::all_registry_descriptors;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

int main() {
    const auto registries = all_registry_descriptors();
    check("at least one registry ships", !registries.empty());

    bool described = true;
    bool identity_matches_token = true;
    for (const auto& descriptor : registries) {
        const std::string token = descriptor.id.str();
        if (token.empty() || descriptor.doc.id != token || descriptor.doc.title.empty() ||
            descriptor.doc.summary.empty() || descriptor.display_name.empty() ||
            descriptor.cache_dir_prefix.empty() || descriptor.revision_semantics.empty() ||
            descriptor.staleness_basis.empty()) {
            described = false;
            std::printf("    [%s] is missing a docs field\n", token.c_str());
        }
        if (descriptor.doc.example.find(token) == std::string::npos) {
            identity_matches_token = false;
            std::printf("    [%s] example does not name its own token\n", token.c_str());
        }
    }
    check("every registry describes itself", described);
    check("a description is filed under its own token", identity_matches_token);

    check("the local origins are a closed set of three",
          lemon::local_origin_tokens().size() == 3);
    check("local_upload is a local origin", lemon::is_local_origin(lemon::local_origin::upload));
    check("a registry token is not a local origin",
          !lemon::is_local_origin("huggingface"));

    const std::filesystem::path scratch = lemon::test::make_scratch_dir("model_sources");
    lemon::utils::set_models_dir((scratch / "models").string());
    ModelManagerConfig config;
    config.cache_dir = scratch.string();
    ModelManager manager(std::move(config));

    const json payload = manager.model_sources();

    json expected_tokens = json::array();
    for (const auto& descriptor : registries) {
        expected_tokens.push_back(descriptor.id.str());
    }
    check("the payload lists every compiled registry",
          payload["source_tokens"] == expected_tokens);

    json expected_pull = expected_tokens;
    for (const std::string& token : lemon::local_origin_tokens()) {
        expected_pull.push_back(token);
    }
    check("/pull accepts registries and local origins, nothing else",
          payload["pull_source_values"] == expected_pull);

    bool catalogs_described = !payload["catalogs"].empty();
    for (const auto& catalog : payload["catalogs"]) {
        if (catalog["title"].get<std::string>().empty() ||
            catalog["summary"].get<std::string>().empty()) {
            catalogs_described = false;
            std::printf("    [%s] is missing a docs field\n",
                        catalog["id"].get<std::string>().c_str());
        }
    }
    check("every catalog that built the set describes itself", catalogs_described);

    bool extra_models_configured = false;
    bool host_rows_flagged = true;
    for (const auto& catalog : payload["catalogs"]) {
        const std::string id = catalog["id"].get<std::string>();
        const bool host = catalog["host_specific"].get<bool>();
        if (id == "extra_models_dir") {
            extra_models_configured = catalog["config_key"] == "extra_models_dir";
        }
        if (id.rfind("discovered:", 0) == 0 || id.rfind("listed:", 0) == 0) {
            host_rows_flagged = host_rows_flagged && host;
        } else {
            host_rows_flagged = host_rows_flagged && !host;
        }
    }
    check("a configurable catalog names its config key", extra_models_configured);
    check("only installed backends and providers vary by host", host_rows_flagged);

    const json huggingface = payload["registries"][0];
    check("a scheme is rendered from the short form", huggingface["scheme"] == "hf://");

    check("the payload has no empty top-level list",
              !payload["registries"].empty() && !payload["local_origin_tokens"].empty());

    if (g_failures == 0) {
        std::printf("All model source descriptor tests passed.\n");
    } else {
        std::printf("%d model source descriptor test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
