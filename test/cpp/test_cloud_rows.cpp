// The cloud rows in a store: what a refresh reseeds, what it refuses to overwrite, and
// what an evict leaves alone. The provider is a callable, so no server is involved.

#include <lemon/model_manager.h>
#include <lemon/models/alias_manager.h>
#include <lemon/models/cloud_rows.h>
#include <lemon/models/store.h>
#include <lemon/models/store_snapshot.h>

#include <algorithm>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using lemon::ModelInfo;
using lemon::models::CloudRows;
using lemon::models::ModelStore;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo listed(const std::string& name, const std::string& provider) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "cloud";
    info.cloud_provider = provider;
    return info;
}

// Publish `rows`, as a build would have.
static void publish(ModelStore& store, std::map<std::string, ModelInfo> rows) {
    ModelStore::Build build(store);
    build.commit(std::move(rows), nullptr, {});
}

static std::vector<std::string> names_of(const ModelStore& store) {
    std::vector<std::string> names;
    for (const auto& [name, info] : store.read()->models) {
        names.push_back(name);
    }
    return names;
}

static void test_refresh_reseeds_one_provider() {
    std::vector<std::vector<ModelInfo>> listings{{listed("alpha", "p1"), listed("beta", "p1")},
                                                 {listed("beta", "p1")}};
    size_t calls = 0;
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    CloudRows rows(store, [&listings, &calls](const std::string&) {
        return listings[std::min(calls++, listings.size() - 1)];
    });

    check("a refresh puts the provider's listing in the store",
          rows.refresh("p1") == 2 && names_of(store).size() == 2);
    check("and counting reads the same rows", rows.count("p1") == 2);

    // The second listing no longer carries alpha.
    check("a refresh drops what the provider stopped listing", rows.refresh("p1") == 1);
    check("the stopped model is gone rather than stale",
          store.read()->find("alpha") == nullptr && store.read()->find("beta") != nullptr);
}

static void test_refresh_leaves_everyone_else_alone() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    publish(store, {
        {"alpha", listed("alpha", "p1")},
        {"other", listed("other", "p2")},
        {"local", [] { ModelInfo info; info.model_name = "local"; info.recipe = "llamacpp"; return info; }()},
    });
    CloudRows rows(store, [](const std::string&) { return std::vector<ModelInfo>{}; });

    check("a provider that lists nothing reseeds to none", rows.refresh("p1") == 0);
    check("another provider's rows survive it", store.read()->find("other") != nullptr);
    check("and so does a local model's", store.read()->find("local") != nullptr);
}

static void test_refresh_does_not_take_a_held_name() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    publish(store, {{"shared", [] {
        ModelInfo info;
        info.model_name = "shared";
        info.recipe = "llamacpp";
        return info;
    }()}});
    CloudRows rows(store, [](const std::string&) {
        return std::vector<ModelInfo>{listed("shared", "p1")};
    });

    check("a name another source holds is not overwritten", rows.refresh("p1") == 0);
    check("the holder keeps its recipe", store.read()->find("shared")->recipe == "llamacpp");
}

static void test_rows_a_listing_left_undressed() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    CloudRows rows(store, [](const std::string&) {
        return std::vector<ModelInfo>{listed("bare", "p1")};
    });
    rows.refresh("p1");

    const ModelInfo* added = store.read()->find("bare");
    check("a row with no options still gets the recipe's own so Router can build it",
          added != nullptr && added->recipe_options.get_recipe() == "cloud");
}

static void test_evict_and_the_empty_ask() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    publish(store, {
        {"alpha", listed("alpha", "p1")},
        {"beta", listed("beta", "p1")},
        {"other", listed("other", "p2")},
    });
    CloudRows rows(store, [](const std::string&) { return std::vector<ModelInfo>{}; });

    check("an evict takes one provider and reports how many", rows.evict("p1") == 2);
    check("the other provider is untouched",
          store.read()->find("other") != nullptr && rows.count("p2") == 1);
    check("evicting nobody answers nothing and changes nothing",
          rows.evict("nope") == 0 && rows.evict("") == 0);
    check("an empty provider name never asks the provider", rows.refresh("") == 0);
    check("and a provider with no way to list is not an error",
          CloudRows(store, nullptr).refresh("p1") == 0);
}

int main() {
    test_refresh_reseeds_one_provider();
    test_refresh_leaves_everyone_else_alone();
    test_refresh_does_not_take_a_held_name();
    test_rows_a_listing_left_undressed();
    test_evict_and_the_empty_ask();

    if (g_failures != 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll cloud row checks passed\n");
    return 0;
}
