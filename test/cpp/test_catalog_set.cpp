#include <lemon/models/catalog.h>
#include <lemon/models/catalog_set.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using lemon::json;
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogEntry;
using lemon::models::DocEntry;
using lemon::models::CatalogDescriptor;
using lemon::models::CatalogSet;
using lemon::models::Conflict;
using lemon::models::EnumeratedCatalog;
using lemon::models::make_catalog_descriptor;
using lemon::models::CatalogSetDeps;
using lemon::models::ModelCatalog;
using lemon::models::product_catalogs;
using lemon::models::RegistryDocuments;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// One catalog claiming `model`, tagged so a winner is identifiable.
static std::shared_ptr<ModelCatalog> fake(const std::string& id, int rank, Conflict claim,
                                          const std::string& tag,
                                          const std::string& model = "Contested") {
    ModelInfo info;
    info.model_name = model;
    info.recipe = tag;
    return std::make_shared<EnumeratedCatalog>(
        make_catalog_descriptor(id, rank, claim,
                                DocEntry{id, tag, "catalog " + id, {}, ""}),
        [info](const CatalogAccumulator&) { return std::vector<ModelInfo>{info}; });
}

static std::string ids(const std::vector<CatalogDescriptor>& descriptors) {
    std::string joined;
    for (const auto& descriptor : descriptors) {
        if (!joined.empty()) joined += ",";
        joined += descriptor.id;
    }
    return joined;
}

static std::string winner_recipe(const CatalogAccumulator& accumulator,
                                 const std::string& model = "Contested") {
    const auto it = accumulator.models().find(model);
    return it == accumulator.models().end() ? std::string("none") : it->second.recipe;
}

static CatalogSetDeps stub_deps() {
    CatalogSetDeps deps;
    deps.pin_registries = [] {
        RegistryDocuments::Pin pin;
        pin.builtin = std::make_shared<const json>(json::object());
        pin.user = std::make_shared<const json>(json::object());
        return pin;
    };
    deps.scan_extra_models = [](const CatalogAccumulator&) { return std::vector<ModelInfo>{}; };
    deps.dynamic_recipes = [] { return std::vector<std::string>{"flm", "cloud"}; };
    deps.discover_backend = [](const std::string&) { return std::vector<ModelInfo>{}; };
    deps.cloud_providers = [] { return std::vector<std::string>{"fireworks"}; };
    deps.list_cloud_models = [](const std::string&) { return std::vector<ModelInfo>{}; };
    return deps;
}

int main() {
    {
        CatalogSet product;
        product.replace_with(product_catalogs(stub_deps()));
        check("the product enumerates registries, then a scan, then what is installed",
              ids(product.descriptors()) ==
                  "server_models.json,user_models.json,extra_models_dir,discovered:flm,"
                  "listed:fireworks");

        bool ranks_ascend = true;
        bool documents_author = true;
        bool host_sources_yield = true;
        int previous = std::numeric_limits<int>::min();
        for (const auto& descriptor : product.descriptors()) {
            ranks_ascend = ranks_ascend && descriptor.rank >= previous;
            previous = descriptor.rank;
            const bool document = descriptor.id == "server_models.json" ||
                                  descriptor.id == "user_models.json";
            documents_author = documents_author &&
                               (descriptor.claim == Conflict::Overwrite) == document;
            host_sources_yield = host_sources_yield &&
                                 (descriptor.claim == Conflict::KeepExisting) != document;
        }
        check("ranks never run backwards in the shipped set", ranks_ascend);
        check("only a document may claim a name it did not read", documents_author);
        check("a scan or listing yields to what it read", host_sources_yield);

        CatalogSetDeps offline = stub_deps();
        offline.offline = true;
        CatalogSet disconnected;
        disconnected.replace_with(product_catalogs(offline));
        check("offline keeps every catalog but the provider listings",
              ids(disconnected.descriptors()) ==
                  "server_models.json,user_models.json,extra_models_dir,discovered:flm");
    }

    {
        CatalogSet set;
        set.add(fake("scan", 20, Conflict::KeepExisting, "scanned"));
        set.add(fake("builtin", 0, Conflict::Overwrite, "builtin"));
        set.add(fake("registrations", 10, Conflict::Overwrite, "registered"));
        check("ranks order enumeration, not the order added",
              ids(set.descriptors()) == "builtin,registrations,scan");
        check("descriptors and snapshot agree on order",
              set.snapshot().front()->descriptor().id == "builtin");

        CatalogSet same_rank;
        same_rank.add(fake("a", 20, Conflict::KeepExisting, "a"));
        same_rank.add(fake("b", 20, Conflict::KeepExisting, "b"));
        check("one rank keeps insertion order",
              ids(same_rank.descriptors()) == "a,b");

        set.add(fake("scan", -5, Conflict::Overwrite, "promoted"));
        check("the same id is replaced, not duplicated", set.size() == 3);
        check("a replacement enumerates at its new rank",
              ids(set.descriptors()) == "scan,builtin,registrations");

        const auto pinned = set.snapshot();
        set.remove("builtin");
        check("a snapshot is not disturbed by a later change", pinned.size() == 3);
        check("remove reports what it did", set.size() == 2 && !set.find("builtin"));
        check("removing what is absent says so", !set.remove("builtin"));

        set.replace_with({fake("late", 40, Conflict::KeepExisting, "late")});
        check("replace_with swaps the whole set",
              set.size() == 1 && ids(set.descriptors()) == "late");
    }

    {
        CatalogSet set;
        set.add(fake("scan", 20, Conflict::KeepExisting, "scanned"));
        set.add(fake("builtin", 0, Conflict::Overwrite, "builtin"));
        set.add(fake("registrations", 10, Conflict::Overwrite, "registered"));

        CatalogAccumulator accumulator;
        for (const auto& catalog : set.snapshot()) {
            catalog->enumerate(accumulator);
        }
        check("a registration still wins a contested name",
              winner_recipe(accumulator) == "registered");
    }

    {
        CatalogSet set;
        set.add(fake("builtin", 0, Conflict::Overwrite, "builtin"));
        set.add(fake("scan", 20, Conflict::KeepExisting, "scanned"));

        CatalogAccumulator accumulator;
        for (const auto& catalog : set.snapshot()) {
            catalog->enumerate(accumulator);
        }
        check("a scan cannot take a name from an earlier rank",
              winner_recipe(accumulator) == "builtin");
    }

    {
        CatalogSet set;
        set.add(fake("scan", 20, Conflict::KeepExisting, "scanned", "Only-Here"));
        CatalogAccumulator accumulator;
        accumulator.add(CatalogEntry{"claimant", [] {
            ModelInfo info;
            info.model_name = "Only-Here";
            info.recipe = "claimed-first";
            return info;
        }()});
        for (const auto& catalog : set.snapshot()) {
            catalog->enumerate(accumulator);
        }
        check("a name claimed before enumeration stands",
              winner_recipe(accumulator, "Only-Here") == "claimed-first");
    }

    if (g_failures == 0) {
        std::printf("All catalog set tests passed.\n");
    } else {
        std::printf("%d catalog set test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
