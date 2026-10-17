// The store's own contract, asked of the store: one snapshot published per publish, one
// writer at a time, a compose that runs outside the writer and yields to a write that
// landed under it, and the catalogs recorded beside the set. No manager and no disk.

#include <lemon/model_manager.h>
#include <lemon/models/catalog.h>
#include <lemon/models/store.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogDescriptor;
using lemon::models::FilterReasons;
using lemon::models::ModelCatalog;
using lemon::models::ModelStore;
using lemon::models::RejectCause;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo model(const std::string& name, bool downloaded = false) {
    ModelInfo info;
    info.model_name = name;
    info.downloaded = downloaded;
    return info;
}

// A catalog that exists and describes itself, and hands over no rows.
class StubCatalog : public ModelCatalog {
  public:
    explicit StubCatalog(const std::string& id) {
        descriptor_.id = id;
    }
    const CatalogDescriptor& descriptor() const override { return descriptor_; }
    void enumerate(CatalogAccumulator&) const override {}

  private:
    CatalogDescriptor descriptor_;
};

static std::vector<std::shared_ptr<ModelCatalog>> no_catalogs() { return {}; }

static std::shared_ptr<const FilterReasons> one_rejection() {
    auto reasons = std::make_shared<FilterReasons>();
    reasons->rejected.push_back({"user.Offline", "needs an NPU", RejectCause::MemoryFit});
    return reasons;
}

static void test_an_undrawn_store() {
    ModelStore store;
    const auto snapshot = store.read();
    check("a reader is never handed nothing", snapshot != nullptr);
    check("and is told to build", !snapshot->valid);
    check("with no models, no catalogs and no generation",
          snapshot->models.empty() && snapshot->catalogs.empty() && store.generation() == 0);

    ModelStore::Build build(store);
    check("an undrawn store is out of date", !build.up_to_date());
    build.commit({{"user.Alpha", model("user.Alpha", true)}, {"Beta", model("Beta")}}, nullptr,
                 {std::make_shared<StubCatalog>("stub")});

    const auto published = store.read();
    check("a build publishes the set it composed", published->valid && published->models.size() == 2);
    check("with the catalogs that produced it recorded beside it",
          published->catalogs.size() == 1 && published->catalogs[0].id == "stub");
    check("and the names derived beside it",
          published->canonical_for("Alpha") == "user.Alpha" &&
          published->public_name_for("user.Alpha") == "Alpha");
    check("the generation rises once per publish", store.generation() == published->generation);

    ModelStore::Build again(store);
    check("a store that answers is not rebuilt", again.up_to_date());
}

static void test_a_patch_publishes_too() {
    ModelStore store;
    ModelStore::Build build(store);
    build.commit({{"user.Alpha", model("user.Alpha")}}, one_rejection(),
                 {std::make_shared<StubCatalog>("stub")});
    const std::uint64_t built = store.generation();

    {
        ModelStore::Write write(store);
        write.draft()["user.Alpha"].downloaded = true;
        write.draft()["user.Added"] = model("user.Added", true);
    }

    const auto patched = store.read();
    check("a write publishes a set with both changes",
          patched->models.size() == 2 && patched->find("user.Alpha")->downloaded);
    check("it publishes exactly once", store.generation() == built + 1);
    check("the rows the patch did not name are alone in the set",
          patched->find("user.Added") != nullptr && patched->models.size() == 2);
    check("a patch says nothing about hardware, so the filter table carries forward",
          patched->reasons && patched->reasons->rejected.size() == 1 &&
              patched->rejection_reason("user.Offline") == "needs an NPU");
    check("and the catalogs carry forward too", patched->catalogs.size() == 1);

    {
        ModelStore::Write write(store);
        write.invalidate_cache();
    }
    check("a change the store cannot patch asks the next read to compose",
          !store.read()->valid);
    // The Build that published first is still in scope: a compose must not hold the
    // builder past its own commit, or a store could never be rebuilt again.
    ModelStore::Build stale(store);
    check("and a second compose may start beside the one that published",
          !stale.up_to_date());
}

static void test_a_write_under_a_build_wins() {
    ModelStore store;
    ModelStore::Build first(store);
    first.commit({{"user.Alpha", model("user.Alpha")}}, nullptr, no_catalogs());

    // Put the store back on a compose: a patch that cannot answer invalidates it.
    {
        ModelStore::Write write(store);
        write.invalidate_cache();
    }
    const std::uint64_t invalidated = store.generation();

    ModelStore::Build compose(store);
    check("the compose starts", !compose.up_to_date());

    // A download lands while the set is being put together.
    std::atomic<bool> written{false};
    std::thread writer([&store, &written] {
        ModelStore::Write write(store);
        write.draft()["user.Alpha"].downloaded = true;
        written = true;
    });
    writer.join();
    check("the write published while the compose ran", written && store.generation() > invalidated);

    check("the candidate that predates it is refused",
          !compose.commit({{"user.Alpha", model("user.Alpha")},
                           {"user.Stale", model("user.Stale")}},
                          nullptr, no_catalogs()));
    const auto settled = store.read();
    check("the write's row stands", settled->find("user.Alpha")->downloaded);
    check("and nothing of the refused set landed", settled->models.count("user.Stale") == 0);
    check("the store stays out of date, so the next read reads the disk again",
          !settled->valid);
}

static void test_a_build_holds_only_the_builder() {
    ModelStore store;
    ModelStore::Build build(store);
    build.commit({{"user.Alpha", model("user.Alpha")}}, nullptr, no_catalogs());

    {
        ModelStore::Write write(store);
        write.invalidate_cache();
    }

    std::atomic<bool> composing{false};
    std::thread builder([&store, &composing] {
        ModelStore::Build slow(store);
        composing = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        slow.commit({{"user.Alpha", model("user.Alpha", true)}}, nullptr, no_catalogs());
    });
    while (!composing.load()) {
        std::this_thread::yield();
    }

    const auto started = std::chrono::steady_clock::now();
    const bool answered = store.read()->models.count("user.Alpha") == 1;
    {
        ModelStore::Write write(store);
        write.draft()["user.Added"] = model("user.Added");
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    builder.join();

    check("a read answers while a compose runs", answered);
    // A write that had to wait for the builder would wait out the compose: the margin
    // is against the compose's own length, not against a machine's speed.
    check("and a write is not made to wait for it", ms < 600);
    check("the compose that raced the write yields to it",
          store.read()->models.count("user.Added") == 1 &&
              !store.read()->find("user.Alpha")->downloaded && !store.read()->valid);
}

int main() {
    test_an_undrawn_store();
    test_a_patch_publishes_too();
    test_a_write_under_a_build_wins();
    test_a_build_holds_only_the_builder();

    if (g_failures != 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll store checks passed\n");
    return 0;
}
