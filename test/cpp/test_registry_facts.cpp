// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include "lemon/model_manager.h"
#include <lemon/models/registry_types.h>
#include "lemon/models/registry_id.h"
#include "lemon/utils/path_utils.h"
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "show_all_models.h"
#include <lemon/model_manager.h>
#include <lemon/models/model_sources.h>
#include <lemon/models/registry_id.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/path_utils.h>
#include "support/test_state_isolation.h"


namespace identity {

using lemon::RegistryId;
using lemon::models::descriptor_for;
using lemon::models::huggingface_descriptor;
using lemon::models::modelscope_descriptor;
using lemon::models::RegistryDescriptor;
using lemon::models::RegistryId;
using lemon::models::registry_id_from;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static void check_eq(const std::string& name, const std::string& actual,
                     const std::string& expected) {
    const bool ok = actual == expected;
    if (!ok) {
        std::printf("       expected \"%s\", got \"%s\"\n", expected.c_str(), actual.c_str());
    }
    check(name, ok);
}

static void test_every_spelling_resolves_to_its_own_host() {
    for (const auto& descriptor : lemon::models::all_registry_descriptors()) {
        for (const auto& alias : descriptor.aliases) {
            const auto resolved = registry_id_from(alias);
            check("alias '" + alias + "' resolves to " + descriptor.id.str(),
                  resolved.has_value() && resolved.value() == descriptor.id);
        }
        std::string mixed = descriptor.id.str();
        if (!mixed.empty()) {
            mixed[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(mixed[0])));
        }
        check("mixed case resolves to " + descriptor.id.str(),
              registry_id_from(mixed).value_or(RegistryId{}) == descriptor.id);
    }
}

static void test_an_unknown_host_resolves_to_nothing() {
    check("an unknown id resolves to nothing", !registry_id_from("gitlfs").has_value());
    check("a near miss resolves to nothing", !registry_id_from("hugging_faces").has_value());
    check("a persisted entry naming an unknown host throws nothing and yields no id",
          !registry_id_from("ModelScop").has_value());
}

static void test_hosts_keep_their_own_namespace_on_disk() {
    const std::string repo = "Qwen/Qwen3-0.6B-GGUF";
    const std::string hf = lemon::registry_repo_cache_dir_name(repo, lemon::models::huggingface_descriptor().id);
    const std::string ms = lemon::registry_repo_cache_dir_name(repo, lemon::models::modelscope_descriptor().id);

    check("one owner/repo cannot collide across hosts", hf != ms);
    check_eq("hugging face keeps its historical directory", hf, "models--Qwen--Qwen3-0.6B-GGUF");
    check_eq("modelscope is namespaced", ms, "modelscope--models--Qwen--Qwen3-0.6B-GGUF");
    check("the directory starts with the host's prefix",
          ms.compare(0, modelscope_descriptor().cache_dir_prefix.size(),
                     modelscope_descriptor().cache_dir_prefix) == 0);
}

static void test_revision_semantics_are_per_host() {
    check_eq("hugging face resolves its default branch", huggingface_descriptor().default_revision,
             "main");
    check_eq("modelscope resolves its default branch", modelscope_descriptor().default_revision,
             "master");
}

// The strings the API and user_models.json persist must come from the table, so
// a host added later needs no change outside it.
static void test_persisted_names_come_from_the_table() {
    check_eq("source name", lemon::remote_registry_source_name(lemon::models::modelscope_descriptor().id),
             modelscope_descriptor().id.str());
    check_eq("display name", lemon::remote_registry_display_name(lemon::models::huggingface_descriptor().id),
             huggingface_descriptor().display_name);
    check_eq("scheme prefix", lemon::remote_registry_scheme_prefix(lemon::models::modelscope_descriptor().id),
             modelscope_descriptor().scheme_prefix);
}

static void test_default_spelling_is_the_default_host() {
    check("an entry with no recorded source is the default host",
          registry_id_from("").value_or(RegistryId{}) == huggingface_descriptor().id);
    check("but naming nothing is not naming a host",
          !lemon::is_remote_registry_source(""));
    check("while an unrecognized name is refused rather than defaulted",
          !registry_id_from("huggingfcae").has_value());
}

// A backend asks whether a host serves plain resolve URLs rather than asking
// which host it is, so the answer has to be a fact the table carries.
static void test_only_a_plain_download_host_offers_a_resolve_url() {
    const RegistryId hf = *registry_id_from("huggingface");
    const RegistryId ms = *registry_id_from("modelscope");

    const RegistryDescriptor* hf_descriptor = descriptor_for(hf);
    const RegistryDescriptor* ms_descriptor = descriptor_for(ms);

    check("the plain download host names an endpoint",
          hf_descriptor != nullptr && !hf_descriptor->endpoint_default.empty());
    check("an API-only host names no endpoint",
          ms_descriptor != nullptr && ms_descriptor->endpoint_default.empty());
    check("a revision comes from the same row as the endpoint",
          hf_descriptor != nullptr && hf_descriptor->default_revision == "main");
}

static void test_lookup_by_id() {
    check("a known id has a descriptor", descriptor_for(modelscope_descriptor().id) != nullptr);
    check("an unknown id has none", descriptor_for(RegistryId("gitlfs")) == nullptr);
}

// A build that no longer knows a registry an entry names must cost that one
// model, not the catalog: the entry contributes nothing and the rest loads.
static void test_a_persisted_entry_naming_an_unknown_host_is_skipped() {
    namespace fs = std::filesystem;
    fs::path temp = fs::temp_directory_path();
    temp /= "registry_identity_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(temp);
    lemon::utils::set_cache_dir(lemon::utils::path_to_utf8(temp));
    lemon::utils::set_config_dir(lemon::utils::path_to_utf8(temp));

    lemon::json models = {
        {"zz-broken",
         {{"checkpoint", "someone/somewhere:main.gguf"},
          {"recipe", "llamacpp"},
          {"registry_source", "gitlfs"}}},
        {"zz-loadable",
         {{"checkpoint", "zzorg/zzloadable:main.gguf"}, {"recipe", "llamacpp"}}},
    };
    fs::path registry_file = temp / "user_models.json";
    std::ofstream out(registry_file);
    out << models.dump(2);
    out.close();

    bool survived = true;
    std::map<std::string, lemon::ModelInfo> listed;
    try {
        lemon::ModelManager manager;
        listed = manager.get_supported_models();
    } catch (const std::exception&) {
        survived = false;
    }

    check("an unknown host in an entry does not stop startup", survived);
    // A precedence winner is listed under its bare name, so accept either form.
    const bool broken_listed =
        listed.count("zz-broken") + listed.count("user.zz-broken") > 0;
    const bool loadable_listed =
        listed.count("zz-loadable") + listed.count("user.zz-loadable") > 0;
    check("the entry naming it contributes nothing", !broken_listed);
    check("the rest of the file still loads", loadable_listed);

    fs::remove_all(temp);
}

int run() {
    ShowAllModels show_all_models;
    test_every_spelling_resolves_to_its_own_host();
    test_an_unknown_host_resolves_to_nothing();
    test_hosts_keep_their_own_namespace_on_disk();
    test_revision_semantics_are_per_host();
    test_persisted_names_come_from_the_table();
    test_default_spelling_is_the_default_host();
    test_only_a_plain_download_host_offers_a_resolve_url();
    test_lookup_by_id();
    test_a_persisted_entry_naming_an_unknown_host_is_skipped();

    if (g_failures == 0) {
        std::printf("All registry identity tests passed.\n");
    } else {
        std::printf("%d registry identity test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace identity


namespace descriptors {

using lemon::json;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::models::all_registry_descriptors;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

int run() {
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

}  // namespace descriptors

int main() {
    int failures = 0;
    std::printf("\n=== identity ===\n");
    failures |= identity::run();
    std::printf("\n=== descriptors ===\n");
    failures |= descriptors::run();
    return failures;
}
