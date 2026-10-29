#include <lemon/model_manager.h>
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

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("composition_root");
    lemon::utils::set_models_dir((scratch / "models").string());

    auto fixture = std::make_shared<lemon::models::EnumeratedCatalog>(
        lemon::models::CatalogDescriptor{"fixture"},
        [](const lemon::models::CatalogAccumulator&) {
            ModelInfo info;
            info.model_name = "Shim-Model";
            info.recipe = "llamacpp";
            info.checkpoints["main"] = "org/shim:a.gguf";
            info.suggested = true;
            return std::vector<ModelInfo>{info};
        });

    const fs::path named = scratch / "named.json";
    {
        std::ofstream out(named, std::ios::binary);
        out << R"({"Configured-Model":{"recipe":"llamacpp","labels":["chat"]}})";
    }

    ModelManagerConfig config;
    config.cache_dir = scratch.string();
    config.extra_catalogs = {fixture};
    config.default_model_source = [] { return "modelscope"; };
    const auto configured = std::make_shared<nlohmann::json>(
        nlohmann::json::array({{{"id", "named"}, {"path", named.string()}}}));
    config.model_catalogs = [configured] { return *configured; };

    ModelManager manager(std::move(config));

    check("the config-injected catalog contributes", manager.model_exists("Shim-Model"));
    check("a catalog named in config.json contributes", manager.model_exists("Configured-Model"));
    check("fixture paths serve the alias manager", manager.aliases().empty());

    const auto resolved = manager.resolve("Shim-Model");
    check("a fixture model resolves", resolved.has_value());
    if (resolved) {
        check("and resolves to its canonical id", resolved->canonical_id == "Shim-Model");
        check("not through an alias", !resolved->via_alias);
        check("and answers under its listed name", resolved->wire_id == "Shim-Model");
    }

    bool read_back = true;
    try {
        const ModelInfo info = manager.get_model_info("Shim-Model");
        check("the configured default source reaches the facade",
              manager.download_source_for(info) ==
                  lemon::models::modelscope_descriptor().id);
    } catch (const std::exception& e) {
        read_back = false;
        std::printf("    [read back threw] %s\n", e.what());
    }
    check("the facade hands the fixture model back", read_back);

    const std::uint64_t generation_before_reads = manager.store().generation();
    manager.store().read();
    manager.store().resolved_paths_for("Shim-Model");
    manager.model_exists("Shim-Model");
    manager.is_model_downloaded("Shim-Model");
    check("a batch of reads leaves the generation where it was",
          manager.store().generation() == generation_before_reads);
    check("a model nobody recorded paths for has none", manager.store().resolved_paths_for("Nobody-Here").empty());

    const fs::path bodies = scratch / "catalogs";
    fs::create_directories(bodies);
    {
        std::ofstream(bodies / "named.json") << "{}";
        std::ofstream(bodies / "orphan.json") << "{}";
        std::ofstream(bodies / "orphan.json.validators") << "{}";
    }

    manager.catalog_set_changed();
    check("a body for a catalog that is still configured survives",
          fs::exists(bodies / "named.json"));
    check("a body for a catalog nobody configures is removed",
          !fs::exists(bodies / "orphan.json") && !fs::exists(bodies / "orphan.json.validators"));

    *configured = nlohmann::json::array();
    manager.catalog_set_changed();
    check("a catalog taken out of config.json stops contributing",
          !manager.model_exists("Configured-Model"));
    check("and what it held is not taken with it", manager.model_exists("Shim-Model"));
    check("its cached body goes with it", !fs::exists(bodies / "named.json"));

    if (g_failures == 0) {
        std::printf("All composition root tests passed.\n");
    } else {
        std::printf("%d composition root test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
