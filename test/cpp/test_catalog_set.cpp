#include <lemon/models/catalog.h>
#include <lemon/models/catalog_set.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogEntry;
using lemon::models::DocEntry;
using lemon::models::CatalogDescriptor;
using lemon::models::CatalogSet;
using lemon::models::Conflict;
using lemon::models::EnumeratedCatalog;
using lemon::models::make_catalog_descriptor;
using lemon::models::ModelCatalog;

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

int main() {
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
