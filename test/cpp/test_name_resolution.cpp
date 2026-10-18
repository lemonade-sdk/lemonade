#include <lemon/model_manager.h>
#include <lemon/models/alias_manager.h>
#include <lemon/models/catalog.h>
#include <lemon/models/registry_descriptors.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/path_utils.h>
#include <nlohmann/json.hpp>

#include "support/test_state_isolation.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using nlohmann::json;

// The chain length the resolver stops at.
constexpr int kChainLimit =
    static_cast<int>(lemon::models::AliasManager::MAX_ALIAS_CHAIN_DEPTH);

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

ModelInfo row(const std::string& name, std::vector<std::string> input_aliases = {}) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "llamacpp";
    info.checkpoints["main"] = "org/" + name + ":a.gguf";
    info.suggested = true;
    info.labels = {"chat"};
    info.input_aliases = std::move(input_aliases);
    return info;
}

// One catalog, three rows: a plain model, a model claiming somebody else's bare
// name as an input alias, and the model whose name it claims.
lemon::models::EnumeratedCatalog::Discover fixture_rows() {
    return [](const lemon::models::CatalogAccumulator&) {
        return std::vector<ModelInfo>{row("Resolution-Model"),
                                      row("Claimant-Model",
                                          {"Contested-Name", "extra.Claimant-Nickname"}),
                                      row("Contested-Name")};
    };
}

// A declared machine, so a build box with no accelerator still publishes the fixture
// rows this test resolves against.
ModelManagerConfig fixture_config(const fs::path& cache_dir) {
    ModelManagerConfig config;
    config.cache_dir = cache_dir.string();
    config.extra_catalogs = {std::make_shared<lemon::models::EnumeratedCatalog>(
        lemon::models::CatalogDescriptor{"resolution"}, fixture_rows())};
    config.availability = [](const std::map<std::string, ModelInfo>&) {
        lemon::models::BackendAvailability declared;
        declared.system_ram_gb = 64.0;
        declared.largest_mem_pool_gb = 48.0;
        return declared;
    };
    return config;
}

bool resolves_to(ModelManager& manager, const std::string& requested,
                 const std::string& expected_canonical) {
    const auto resolved = manager.resolve(requested);
    return resolved && resolved->canonical_id == expected_canonical;
}

// Alias keys a1..aN, the last pointing at the model. Set end-first so each
// binding's own chain check walks a chain that already terminates.
void chain_to(ModelManager& manager, const std::string& first, int links) {
    std::string target = "Resolution-Model";
    for (int link = links; link >= 1; --link) {
        const std::string alias = first + "-" + std::to_string(link);
        std::string err;
        if (!manager.set_alias(alias, target, err)) {
            std::printf("    [chain set failed at %s] %s\n", alias.c_str(), err.c_str());
        }
        target = alias;
    }
}

}  // namespace

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("name_resolution");
    lemon::utils::set_models_dir((scratch / "models").string());

    ModelManager manager(fixture_config(scratch));

    const auto plain = manager.resolve("Resolution-Model");
    check("a model resolves under its own name",
          plain && plain->canonical_id == "Resolution-Model" && !plain->via_alias);
    check("and is answered with the name it is listed under",
          plain && plain->wire_id == "Resolution-Model");
    check(":latest is the same model",
          resolves_to(manager, "Resolution-Model:latest", "Resolution-Model"));

    check("an input alias resolves to the model that claimed it",
          resolves_to(manager, "Contested-Name", "Claimant-Model"));
    check("and the model it displaced keeps its canonical id",
          resolves_to(manager, "builtin.Contested-Name", "Contested-Name"));
    check("a prefixed input alias resolves too",
          resolves_to(manager, "extra.Claimant-Nickname", "Claimant-Model"));
    bool listed_under_alias = false;
    for (const auto& [key, info] : manager.all_models()) {
        static_cast<void>(info);
        if (manager.get_public_model_name(key) == "Contested-Name") listed_under_alias = true;
    }
    check("an input alias is accepted but never listed", !listed_under_alias);

    std::string err;
    check("a user alias is accepted", manager.set_alias("my-shortcut", "Resolution-Model", err));
    const auto via_alias = manager.resolve("my-shortcut");
    check("and resolves to its target",
          via_alias && via_alias->canonical_id == "Resolution-Model" && via_alias->via_alias);
    check("answered with the alias the client asked for",
          via_alias && via_alias->wire_id == "my-shortcut");

    check("a name that exists beats an alias pointing elsewhere",
          manager.set_alias("Resolution-Model", "Claimant-Model", err));
    check("so the snapshot answers, not the alias",
          resolves_to(manager, "Resolution-Model", "Resolution-Model"));
    manager.remove_alias("Resolution-Model");

    check("an alias to a model nobody has yields no target",
          manager.set_alias("dangling", "Nowhere-Model", err) &&
              !manager.resolve("dangling").has_value());

    chain_to(manager, "chain", kChainLimit);
    const auto chained = manager.resolve("chain-1");
    check("a chain of the maximum length still resolves",
          chained && chained->canonical_id == "Resolution-Model" && chained->via_alias);
    check("and answers with the alias it was asked under",
          chained && chained->wire_id == "chain-1");

    chain_to(manager, "deep", kChainLimit + 1);
    check("a chain still naming an alias at the limit yields no target",
          !manager.resolve("deep-1").has_value());
    // The end-to-end check above cannot tell a refused chain from a chain that ran off
    // the map, so ask the map itself.
    check("and the alias map refuses it rather than answering mid-chain",
          !manager.resolve_alias("deep-1").has_value());

    check("a cycle is refused where it closes",
          manager.set_alias("ring-a", "ring-b", err) &&
              !manager.set_alias("ring-b", "ring-a", err) && !err.empty());
    check("so the binding that would close it was never stored",
          manager.aliases().count("ring-b") == 0);

    // A ring that arrives from disk never enters the map, so the guard is not
    // only the one at set time.
    const fs::path planted = scratch / "planted";
    fs::create_directories(planted);
    {
        std::ofstream out(planted / "aliases.json", std::ios::binary);
        out << R"({"p-a":"p-b","p-b":"p-a","p-ok":"Resolution-Model"})";
    }
    ModelManager from_disk(fixture_config(planted));
    check("a ring loaded from disk is pruned", from_disk.aliases().size() == 1);
    check("and nothing on the ring resolves", !from_disk.resolve("p-a").has_value());
    check("a sound binding beside it survives",
          resolves_to(from_disk, "p-ok", "Resolution-Model"));

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll name-resolution checks passed\n");
    return 0;
}
