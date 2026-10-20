#include <lemon/canonical_id.h>
#include <lemon/model_manager.h>
#include <lemon/models/catalog.h>
#include <lemon/models/entry_identity.h>
#include <lemon/models/listed_catalog.h>
#include <lemon/models/registry_descriptors.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/path_utils.h>
#include <nlohmann/json.hpp>

#include "support/test_state_isolation.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::ModelSource;
using nlohmann::json;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

// A GGUF the scanner will take, in the layout it reads a repository from.
void stub_gguf(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const char header[] = {'G', 'G', 'U', 'F', 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    out.write(header, sizeof(header));
}

ModelInfo row(const std::string& name) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "llamacpp";
    info.checkpoints["main"] = "org/" + name + ":a.gguf";
    info.suggested = true;
    info.labels = {"chat"};
    return info;
}

}  // namespace

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("identity_round_trip");
    lemon::utils::set_models_dir((scratch / "models").string());
    const fs::path cache = scratch / "cache";
    const fs::path extra = scratch / "extra";
    fs::create_directories(cache);
    // The registries live in the config dir, which the test sandbox points at HOME.
    lemon::utils::set_config_dir(cache.string());

    {
        std::ofstream out(cache / "user_models.json", std::ios::binary);
        out << R"({"Mine-Model":{"recipe":"llamacpp","checkpoints":{"main":"org/mine:a.gguf"}}})";
    }
    stub_gguf(extra / "models--lemon-test--Ident-Extra-GGUF" / "snapshots" / "active" /
              "Ident-Extra-Q4_K_M.gguf");
    fs::create_directories(extra / "models--lemon-test--Ident-Extra-GGUF" / "refs");
    {
        std::ofstream out(extra / "models--lemon-test--Ident-Extra-GGUF" / "refs" / "main");
        out << "active";
    }

    ModelInfo cloud = row("identprov.Prov-Model");
    cloud.cloud_provider = "identprov";

    ModelManagerConfig config;
    config.cache_dir = cache.string();
    config.extra_models_dir = extra.string();
    config.extra_catalogs = {
        std::make_shared<lemon::models::EnumeratedCatalog>(
            lemon::models::CatalogDescriptor{"identity"},
            [](const lemon::models::CatalogAccumulator&) {
                return std::vector<ModelInfo>{row("Ident-Builtin")};
            }),
        std::make_shared<lemon::models::ListedCatalog>("identprov",
                                                       [cloud] { return std::vector<ModelInfo>{cloud}; }),
    };
    // The identity of a row is the question, not whether this machine could run it.
    config.availability = [](const std::map<std::string, ModelInfo>&) {
        lemon::models::BackendAvailability declared;
        declared.system_ram_gb = 64.0;
        declared.largest_mem_pool_gb = 48.0;
        return declared;
    };
    ModelManager manager(std::move(config));
    // A read that composes; the readers below take the published set as it stands.
    static_cast<void>(manager.get_supported_models());
    const auto models = manager.store().read()->models;
    // Named rows rather than a count: a build box without accelerators publishes
    // fewer built-ins, and that says nothing about identity.
    check("the fixture rows are all published",
          models.count("Ident-Builtin") && models.count("user.Mine-Model") &&
              models.count("identprov.Prov-Model") &&
              std::any_of(models.begin(), models.end(), [](const auto& entry) {
                  return entry.first.find("Ident-Extra") != std::string::npos;
              }));

    bool has_registered = false;
    bool has_imported = false;
    bool has_builtin = false;
    bool qualifies_itself = false;
    bool unparsable_id = false;
    bool bare_mismatch = false;
    bool reserved_mismatch = false;
    std::set<std::string> idempotent_ok;
    for (const auto& [key, info] : models) {
        const lemon::models::EntryIdentity identity = lemon::models::entry_identity(key, info);
        qualifies_itself = qualifies_itself || identity.qualifies_itself;
        const std::string id = lemon::cache_key_to_canonical_id(key);
        const auto parsed = lemon::parse_canonical_id(id);
        if (!parsed || parsed->full() != id) {
            unparsable_id = true;
            std::printf("    [%s] minted %s\n", key.c_str(), id.c_str());
            continue;
        }
        if (parsed->bare_name != identity.bare) bare_mismatch = true;
        if (parsed->source == ModelSource::Registered) has_registered = true;
        if (parsed->source == ModelSource::Imported) has_imported = true;
        if (parsed->source == ModelSource::Builtin) has_builtin = true;
        // What a listing mints for one source must be creatable by the flow that
        // owns that source, and refused by the flows that do not.
        if (lemon::is_reserved_registration_name(id) !=
            (parsed->source != ModelSource::Registered)) {
            reserved_mismatch = true;
            std::printf("    [%s] reserved for %d\n", id.c_str(), static_cast<int>(parsed->source));
        }
        if (lemon::cache_key_to_canonical_id(id) == id) idempotent_ok.insert(key);
    }

    check("the fixture set covers a registered row", has_registered);
    check("a discovered row", has_imported);
    check("a built-in row", has_builtin);
    check("and a provider that qualifies its own id", qualifies_itself);
    check("every minted id parses back to itself", !unparsable_id);
    check("and its bare name survives the prefix", !bare_mismatch);
    check("only a registered id is registrable", !reserved_mismatch);
    check("minting an id twice changes nothing", idempotent_ok.size() == models.size());

    bool name_escapes = false;
    std::map<std::string, std::string> listed;
    for (const auto& [key, info] : models) {
        static_cast<void>(info);
        const std::string public_name = manager.get_public_model_name(key);
        if (public_name.empty()) continue;
        if (listed.count(public_name) && listed[public_name] != key) {
            name_escapes = true;
            std::printf("    [%s] shares %s with %s\n", key.c_str(), public_name.c_str(),
                        listed[public_name].c_str());
        }
        listed[public_name] = key;
        const auto resolved = manager.resolve(public_name);
        if (!resolved || resolved->canonical_id != key) {
            name_escapes = true;
            std::printf("    [%s] listed as %s, resolves to %s\n", key.c_str(), public_name.c_str(),
                        resolved ? resolved->canonical_id.c_str() : "<nothing>");
        }
    }
    check("every listed name answers with the row it names", !name_escapes);

    check("a bare name is not a canonical id",
          !lemon::parse_canonical_id("My-Model").has_value());
    check("a registered id is registrable",
          !lemon::is_reserved_registration_name("user.My-Model"));
    check("a discovered id is not", lemon::is_reserved_registration_name("extra.My-Model"));
    check("a built-in id is not", lemon::is_reserved_registration_name("builtin.My-Model"));
    check("nor is a registered id hiding one",
          lemon::is_reserved_registration_name("user.extra.My-Model") &&
              lemon::is_reserved_registration_name("user.builtin.My-Model"));

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll identity round-trip checks passed\n");
    return 0;
}
