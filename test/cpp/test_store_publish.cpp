// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include <lemon/model_manager.h>
#include <lemon/model_types.h>
#include <lemon/models/alias_manager.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/catalog.h>
#include <lemon/models/store.h>
#include <lemon/models/store_builder.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/path_utils.h>
#include "support/test_state_isolation.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "lemon/model_manager.h"
#include "lemon/models/alias_manager.h"
#include "lemon/models/store.h"
#include "lemon/models/store_snapshot.h"
#include "lemon/utils/path_utils.h"
#include "support/server_wired_manager.h"
#include <fstream>
#include <set>
#include "show_all_models.h"
#include <lemon/models/registry_descriptors.h>
#include <stdexcept>

// The write side of the store, asked of the store itself: publish, patch, invalidate, and
// what readers see while each is happening. Three files used to cover this as separate
// binaries; they are one process now, each keeping its own helpers in its own namespace.
namespace write {

// The write side of the store, asked of the store itself: publish, patch, invalidate, and
// what readers see while each is happening. Three files used to cover this as separate
// binaries; they are one process now, each keeping its own helpers in its own namespace.




using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::RecipeOptions;
using lemon::models::ArtifactSource;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogDescriptor;
using lemon::models::EnumeratedCatalog;
using lemon::models::FilterReasons;
using lemon::models::ModelArtifactStore;
using lemon::models::ModelCatalog;
using lemon::models::ModelStore;
using lemon::models::RejectCause;
using nlohmann::json;

static int g_failures = 0;


namespace store_publish {
// ---- Publish, patch, invalidate: from test_store.cpp
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogDescriptor;
using lemon::models::FilterReasons;
using lemon::models::ModelCatalog;
using lemon::models::ModelStore;
using lemon::models::RejectCause;

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
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
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

// A writer holds the store while a reader asks for the set. The published snapshot is a
// shared_ptr taken independently of the writer, so the reader answers from it instead of
// queueing behind the lock.
static void test_a_read_answers_while_a_writer_holds() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    ModelStore::Build build(store);
    build.commit({{"user.Alpha", model("user.Alpha")}}, one_rejection(),
                 {std::make_shared<StubCatalog>("stub")});

    std::atomic<bool> holds{false};
    std::atomic<bool> timed{false};
    std::thread writer([&store, &holds, &timed] {
        ModelStore::Write write(store);
        holds = true;
        // Held until the read has been timed, so the two are guaranteed to overlap; the
        // deadline is only what stops the test hanging if they never do.
        const auto bail = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!timed.load() && std::chrono::steady_clock::now() < bail) {
            std::this_thread::yield();
        }
    });
    while (!holds.load()) {
        std::this_thread::yield();
    }

    const auto started = std::chrono::steady_clock::now();
    const auto published = store.read();
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    timed = true;
    writer.join();

    check("a read answers while a writer holds the store", microseconds < 150000);
    check("and answers with the set published before the write",
          published && published->find("user.Alpha") != nullptr);
}

static void test_a_patch_publishes_too() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
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
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
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

    ModelStore::Build retry(store);
    std::map<std::string, ModelInfo> recomposed;
    recomposed["user.Alpha"] = *settled->find("user.Alpha");
    recomposed["user.Fresh"] = model("user.Fresh");
    check("a compose asked again lands on the write",
          retry.commit(std::move(recomposed), nullptr, no_catalogs()));
    const auto landed = store.read();
    check("and carries the write with it",
          landed->valid && landed->find("user.Alpha")->downloaded &&
              landed->models.count("user.Fresh") == 1);
}

static void test_a_build_holds_only_the_builder() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    ModelStore::Build build(store);
    build.commit({{"user.Alpha", model("user.Alpha")}}, nullptr, no_catalogs());

    {
        ModelStore::Write write(store);
        write.invalidate_cache();
    }

    std::atomic<bool> composing{false};
    std::atomic<bool> raced{false};
    std::thread builder([&store, &composing, &raced] {
        ModelStore::Build slow(store);
        composing = true;
        // The compose stays open until the read and the write below have run, so the
        // race it exists to test always happens. The deadline bounds a hang.
        const auto bail = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!raced.load() && std::chrono::steady_clock::now() < bail) {
            std::this_thread::yield();
        }
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
    raced = true;
    builder.join();

    check("a read answers while a compose runs", answered);
    // The compose is still held open here, so a write that queued behind it would take
    // as long as the hold's deadline rather than this margin.
    check("and a write is not made to wait for it", ms < 600);
    check("the compose that raced the write yields to it",
          store.read()->models.count("user.Added") == 1 &&
              !store.read()->find("user.Alpha")->downloaded && !store.read()->valid);
}

// The set did not change when it was marked stale, so nothing may claim it did: the
// generation is how a slow reader learns its snapshot is no longer current, and a bump
// that came from bookkeeping rather than from a change made that signal lie.
// A writer that persists outside the store invalidates it rather than patching it, so it
// publishes nothing and the generation stays where it was. A compose in flight has to notice
// anyway, or it lands a set built before that persist and calls it valid.
static void test_an_invalidation_under_a_build_wins() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    ModelStore::Build first(store);
    first.commit({{"user.Alpha", model("user.Alpha")}}, nullptr, no_catalogs());

    // One invalidation puts the store back on a compose; the one below is the persist that
    // lands while that compose is reading the registries.
    store.invalidate();
    ModelStore::Build compose(store);
    check("the compose starts", !compose.up_to_date());
    store.invalidate();
    check("a persist that published nothing still refuses the older candidate",
          !compose.commit({{"user.Alpha", model("user.Alpha", true)}}, nullptr, no_catalogs()));
    // The refused candidate marked the row downloaded; the published set must not carry it.
    const auto settled = store.read();
    check("and the refused candidate never became the published set",
          settled->find("user.Alpha") != nullptr && !settled->find("user.Alpha")->downloaded);
}

static void test_invalidation_publishes_nothing() {
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
    ModelStore::Build build(store);
    build.commit({{"user.Alpha", model("user.Alpha")}}, nullptr, no_catalogs());
    const std::uint64_t built = store.generation();

    store.invalidate();
    store.invalidate();

    check("invalidating publishes no new generation", store.generation() == built);

    ModelStore::Build compose(store);
    check("the next read is asked to compose", !compose.up_to_date());
    check("and readers still hold the set that was there", store.read()->models.size() == 1);
    compose.commit({{"user.Alpha", model("user.Alpha")}, {"user.Beta", model("user.Beta")}},
                   nullptr, no_catalogs());
    check("and the rebuild advances the generation exactly once",
          store.generation() == built + 1);

    ModelStore::Build settled(store);
    check("a settled store is not composed again", settled.up_to_date());
}

}  // namespace store_publish

namespace store_rebuild {
// ---- Compose under a writer: from test_store_build.cpp
namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::RecipeOptions;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogDescriptor;
using lemon::models::EnumeratedCatalog;
using nlohmann::json;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// A catalog that reports how many times it was read and can be held inside one
// read, which is where a rebuild spends nearly all of its time.
class HeldCatalog {
  public:
    explicit HeldCatalog(std::string id, std::vector<ModelInfo> rows)
        : id_(std::move(id)), rows_(std::move(rows)) {}

    std::shared_ptr<EnumeratedCatalog> make() {
        return std::make_shared<EnumeratedCatalog>(
            CatalogDescriptor{id_},
            [this](const CatalogAccumulator&) {
                ++reads;
                std::unique_lock<std::mutex> lock(mutex_);
                if (!armed) return rows_;
                inside = true;
                arrived_.notify_all();
                released_.wait(lock, [this] { return released; });
                return rows_;
            });
    }

    // Every read then stops inside, so a reader that reached this catalog instead
    // of waiting behind the builder cannot slip past unnoticed.
    void arm() {
        std::lock_guard<std::mutex> lock(mutex_);
        armed = true;
        inside = false;
        released = false;
    }

    void wait_until_inside() {
        std::unique_lock<std::mutex> lock(mutex_);
        arrived_.wait(lock, [this] { return inside; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            armed = false;
            released = true;
        }
        released_.notify_all();
    }

    std::atomic<int> reads{0};

  private:
    std::string id_;
    std::vector<ModelInfo> rows_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::condition_variable released_;
    bool armed = false;
    bool inside = false;
    bool released = false;
};

static ModelInfo row(const std::string& name) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "llamacpp";
    info.labels = {"chat"};
    info.checkpoints["main"] = "org/" + name + ":main.gguf";
    return info;
}

static ModelManagerConfig fixture(HeldCatalog& held) {
    ModelManagerConfig config;
    const fs::path scratch = lemon::test::make_scratch_dir("store_build");
    lemon::utils::set_models_dir((scratch / "models").string());
    config.cache_dir = scratch.string();
    config.extra_catalogs = {held.make()};
    return config;
}

// A writer must not wait for a rebuild that is only reading catalogs and disk.
static void test_a_writer_answers_during_a_rebuild() {
    HeldCatalog held("held", {row("Held-Model")});
    ModelManager manager(fixture(held));
    held.arm();

    std::thread reader([&manager] { manager.get_supported_models(); });
    held.wait_until_inside();

    auto writer = std::async(std::launch::async, [&manager] {
        manager.invalidate_models_cache();
    });
    const bool answered =
        writer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

    held.release();
    writer.get();
    reader.join();

    check("a writer answers while a rebuild composes", answered);
    check("the rebuild still finishes and lists what it read",
          manager.model_exists("Held-Model"));
}

// A write that lands mid-composition outranks the candidate that predates it.
static void test_a_write_during_a_rebuild_survives_it() {
    HeldCatalog held("raced", {row("Raced-Base")});
    ModelManager manager(fixture(held));

    check("the store is built before the race", manager.model_exists("Raced-Base"));

    manager.invalidate_models_cache();
    held.arm();
    std::thread reader([&manager] { manager.get_supported_models(); });
    held.wait_until_inside();

    // Bounded wait on a side thread: a build that held the store lock across its
    // composition fails this instead of hanging it.
    auto registration = std::async(std::launch::async, [&manager] {
        manager.register_user_model("user.Raced",
                                    json{{"model_name", "user.Raced"},
                                         {"recipe", "llamacpp"},
                                         {"checkpoint", "org/raced:main.gguf"}});
    });
    const bool answered =
        registration.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

    held.release();
    registration.get();
    reader.join();

    check("a registration landing during a rebuild is not blocked by it", answered);

    // The candidate was composed before the registration existed, so it yields
    // and the next read rebuilds with the registration in it.
    const auto models = manager.get_supported_models();
    const bool listed = models.count("user.Raced") == 1 || models.count("Raced") == 1;
    check("a model registered during a rebuild is listed", listed);
    check("and the rebuild that yielded did not leave the store short",
          manager.model_exists("Raced-Base"));
}

// The reasons a model is not offered belong to the set published with them.
static void test_the_commit_publishes_the_filter_reasons() {
    ModelInfo absent = row("Absent-Model");
    absent.recipe = "no-such-backend-xyz";
    HeldCatalog held("reasons", {row("Present-Model"), absent});
    ModelManager manager(fixture(held));

    const auto models = manager.get_supported_models();
    check("a model whose backend is absent is not listed",
          models.count("Present-Model") == 1 && models.count("Absent-Model") == 0);
    check("the rebuild published why it is not listed",
          !manager.get_model_filter_reason("Absent-Model").empty());

    // A patch says nothing about hardware fit, so the table the rebuild computed
    // is the one that stays published.
    ModelInfo present = manager.get_model_info("Present-Model");
    present.recipe_options = RecipeOptions("llamacpp", json{{"ctx_size", 1024}});
    manager.replace_options("Present-Model", present.recipe_options.to_json());
    check("a patch to one row keeps the table of what the host cannot run",
          !manager.get_model_filter_reason("Absent-Model").empty());
}

// Rebuilds serialize, so concurrent readers pay for one.
static void test_concurrent_readers_compose_once() {
    HeldCatalog held("shared", {row("Shared-Model")});
    ModelManager manager(fixture(held));
    held.arm();

    std::thread first([&manager] { manager.get_supported_models(); });
    held.wait_until_inside();
    std::thread second([&manager] { manager.get_supported_models(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    held.release();
    first.join();
    second.join();

    check("two readers cold-starting read the catalogs once", held.reads == 1);
    check("and both see the composed set", manager.model_exists("Shared-Model"));
}

// A download landing patches the collections that carry the model. That decision
// is a pure function of the published set, so it is tested as one.
static ModelInfo row(const std::string& name, bool downloaded,
                     std::vector<std::string> components = {}) {
    ModelInfo info;
    info.model_name = name;
    info.downloaded = downloaded;
    info.components = std::move(components);
    if (!info.components.empty()) info.recipe = lemon::COLLECTION_ROUTER_MODEL_RECIPE;
    return info;
}

static bool reports(const std::vector<std::pair<std::string, bool>>& changed,
                    const std::string& name, bool state) {
    for (const auto& [model, value] : changed) {
        if (model == name) return value == state;
    }
    return false;
}

static void test_collections_follow_their_components() {
    std::map<std::string, ModelInfo> rows = {
        {"coll", row("coll", false, {"comp", "other"})},
        {"comp", row("comp", false)},
        {"other", row("other", true)},
    };

    const auto landed = lemon::models::collection_states_after_component(rows, "comp", true);
    check("a collection completes when its last component lands", reports(landed, "coll", true));

    check("a collection already down is not reported",
          lemon::models::collection_states_after_component(rows, "comp", false).empty());

    check("a model no collection carries changes nothing",
          lemon::models::collection_states_after_component(rows, "stranger", true).empty());

    rows["coll"] = row("coll", false, {"comp", "ghost"});
    check("a collection missing a component it never had stays down",
          lemon::models::collection_states_after_component(rows, "comp", true).empty());

    rows["ghost"] = row("ghost", true);
    rows["coll"] = row("coll", true, {"comp", "ghost"});
    const auto lost = lemon::models::collection_states_after_component(rows, "comp", false);
    check("a collection up for its components comes down when one goes",
          reports(lost, "coll", false));

    std::map<std::string, ModelInfo> plain_rows = {
        {"plain", row("plain", false)},
    };
    plain_rows["plain"].recipe = "llamacpp";
    plain_rows["plain"].components = {"comp"};
    check("a row that is not a collection is not treated as one",
          lemon::models::collection_states_after_component(plain_rows, "comp", true).empty());
}

// The names a reader is given are derived when the store publishes, so a write that
// changes the set cannot leave a name pointing at a model that is no longer there.
static void test_the_published_names_follow_the_set() {
    HeldCatalog held("names", {row("Alpha")});
    ModelManager manager(fixture(held));
    manager.get_supported_models();

    check("the built-in answers to its bare name",
          manager.resolve_model_name("Alpha") == "Alpha");

    manager.register_user_model("user.Alpha", json{{"model_name", "user.Alpha"},
                                                   {"recipe", "llamacpp"},
                                                   {"checkpoint", "someone/Alpha:main.gguf"}});
    manager.get_supported_models();  // the registration invalidated the set
    check("the registration wins the bare name",
          manager.resolve_model_name("Alpha") == "user.Alpha");

    manager.delete_model("user.Alpha");
    check("with the registration gone the built-in answers to the bare name again",
          manager.resolve_model_name("Alpha") == "Alpha");
    check("nothing resolves to the deleted model", !manager.model_exists("user.Alpha"));
}

// A write seeds its draft from the set readers hold, so patching one row cannot drop
// what an earlier write put into the set.
static void test_a_write_seeds_from_what_readers_hold() {
    HeldCatalog held("seed", {row("Alpha"), row("Beta")});
    ModelManager manager(fixture(held));
    manager.get_supported_models();

    manager.register_user_model("user.Gamma", json{{"model_name", "user.Gamma"},
                                                   {"recipe", "llamacpp"},
                                                   {"checkpoint", "org/gamma:main.gguf"}});
    manager.get_supported_models();  // the registration invalidated the set
    check("the registration is in the published set", manager.model_exists("user.Gamma"));

    ModelInfo alpha = manager.get_model_info("Alpha");
    alpha.recipe_options = RecipeOptions("llamacpp", json{{"ctx_size", 2048}});
    manager.replace_options("Alpha", alpha.recipe_options.to_json());  // one row changes
    check("a patch to one row leaves the rest of the set published",
          manager.model_exists("Alpha") && manager.model_exists("Beta") &&
              manager.model_exists("user.Gamma"));
}

}  // namespace store_rebuild

namespace store_patch {
// ---- One row moving: from test_store_patches.cpp
using lemon::ModelInfo;
using lemon::RecipeOptions;
using lemon::models::ArtifactSource;
using lemon::models::ModelArtifactStore;
using lemon::models::ModelStore;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo row(const std::string& key, const std::string& recipe = "llamacpp") {
    ModelInfo info;
    info.model_name = key;
    info.recipe = recipe;
    info.labels = {"chat"};
    info.max_context_window = 4096;
    return info;
}

static void publish(ModelStore& store, std::map<std::string, ModelInfo> rows) {
    ModelStore::Build build(store);
    build.commit(std::move(rows), nullptr, {});
}

// An artifact store pointed at a directory with nothing in it.
struct Fixture {
    std::filesystem::path cache_dir = std::filesystem::temp_directory_path() / "store_patch_cache";
    ModelArtifactStore artifacts{ModelArtifactStore::Hooks{
        [this] { return cache_dir.string(); },
        [](const ModelInfo&, const std::string&) -> std::unique_ptr<ArtifactSource> {
            return nullptr;
        },
        [] { return std::string(); },
    }};
    lemon::models::AliasManager aliases{""};
    ModelStore store{aliases};
};

static void test_a_download_moves_its_row() {
    Fixture f;
    ModelInfo here = row("Alpha");
    here.update_available = true;
    publish(f.store, {{"Alpha", here}});

    f.store.patch_download(f.artifacts, "Alpha", true);
    const auto published = f.store.read();
    check("a download marks the row present", published->find("Alpha")->downloaded);
    check("and a model whose bytes just arrived is not also behind",
          !published->find("Alpha")->update_available);

    f.store.patch_download(f.artifacts, "Alpha", false);
    check("going the other way marks it absent", !f.store.read()->find("Alpha")->downloaded);
    check("and the context it read from disk goes with the bytes",
          f.store.read()->find("Alpha")->max_context_window == 0);
}

static void test_a_model_that_is_not_here() {
    Fixture f;
    publish(f.store, {{"Alpha", row("Alpha")}});
    const std::uint64_t generation = f.store.generation();

    f.store.patch_download(f.artifacts, "Nobody-Here", true);
    check("a name nobody recorded changes nothing, and publishes nothing",
          f.store.generation() == generation);
}

static void test_a_collection_follows_its_component() {
    Fixture f;
    ModelInfo component = row("Classifier");
    component.downloaded = true;
    ModelInfo kit = row("RouterKit", "collection.router");
    kit.downloaded = true;
    kit.components = {"Classifier"};
    publish(f.store, {{"Classifier", component}, {"RouterKit", kit}});

    f.store.patch_download(f.artifacts, "Classifier", false);
    check("a collection that routes through a model that left says it is not here either",
          !f.store.read()->find("RouterKit")->downloaded);

    f.store.patch_download(f.artifacts, "Classifier", true);
    check("and it comes back when the component does",
          f.store.read()->find("RouterKit")->downloaded);
}

static ModelInfo collection(const std::string& key, std::vector<std::string> components) {
    ModelInfo kit = row(key, lemon::COLLECTION_ROUTER_MODEL_RECIPE);
    kit.components = std::move(components);
    return kit;
}

// Both writers have to be inside patch_download at once to lose the collection's state, so
// they start together and the round repeats: the window is filesystem work wide.
static void test_two_components_landing_together() {
    constexpr int kRounds = 20;
    int rounds_left_behind = 0;

    for (int round = 0; round < kRounds; ++round) {
        Fixture f;
        publish(f.store, {
            {"coll", collection("coll", {"comp.A", "comp.B"})},
            {"comp.A", row("comp.A")},
            {"comp.B", row("comp.B")},
        });

        std::mutex gate_mutex;
        std::condition_variable gate;
        int arrived = 0;
        auto enter_gate = [&] {
            std::unique_lock<std::mutex> lock(gate_mutex);
            if (++arrived == 2) {
                gate.notify_all();
            } else {
                gate.wait(lock, [&] { return arrived == 2; });
            }
        };

        std::thread first([&] {
            enter_gate();
            f.store.patch_download(f.artifacts, "comp.A", true);
        });
        std::thread second([&] {
            enter_gate();
            f.store.patch_download(f.artifacts, "comp.B", true);
        });
        first.join();
        second.join();

        const auto published = f.store.read();
        if (!published->find("comp.A")->downloaded || !published->find("comp.B")->downloaded ||
            !published->find("coll")->downloaded) {
            ++rounds_left_behind;
        }
    }

    check("a collection whose two components landed together says it is here",
          rounds_left_behind == 0);
}

static void test_what_a_delete_takes_and_leaves() {
    Fixture f;
    ModelInfo tool = row("user.Tool");
    tool.source = "user";
    ModelInfo builtin = row("builtin.Model");
    builtin.downloaded = true;
    ModelInfo uploaded = row("Uploaded");
    uploaded.source = "local_upload";
    publish(f.store, {{"user.Tool", tool}, {"builtin.Model", builtin}, {"Uploaded", uploaded}});

    f.store.patch_removed("user.Tool");
    check("a registration leaves the set: no catalog would list it again",
          f.store.read()->find("user.Tool") == nullptr);
    f.store.patch_removed("Uploaded");
    check("so does a local upload, which no catalog lists either",
          f.store.read()->find("Uploaded") == nullptr);

    f.store.patch_removed("builtin.Model");
    const ModelInfo* kept = f.store.read()->find("builtin.Model");
    check("a built-in stays, since the registry still lists it", kept != nullptr);
    check("and says it is not here rather than that it never was",
          kept && !kept->downloaded && !kept->update_available);
}

static void test_options_land_on_the_live_row() {
    std::map<std::string, ModelInfo> draft;
    draft["Alpha"] = row("Alpha");

    ModelInfo re_layered = row("Alpha");
    re_layered.recipe_options = RecipeOptions("llamacpp", lemon::json{{"ctx_size", 8192}});
    ModelStore::patch_options(draft, re_layered);
    check("the re-layered options are on the row a request will read",
          draft["Alpha"].recipe_options.get_explicit_option("ctx_size") == 8192);

    ModelInfo absent = row("Gone");
    absent.recipe_options = RecipeOptions("llamacpp", lemon::json{{"ctx_size", 1024}});
    ModelStore::patch_options(draft, absent);
    check("a row that is not in the set is left alone rather than invented",
          draft.count("Gone") == 0 && draft.size() == 1);
}

}  // namespace store_patch


int run() {
        // publish, patch, invalidate
    store_publish::test_an_undrawn_store();
    store_publish::test_a_read_answers_while_a_writer_holds();
    store_publish::test_a_patch_publishes_too();
    store_publish::test_a_write_under_a_build_wins();
    store_publish::test_an_invalidation_under_a_build_wins();
    store_publish::test_invalidation_publishes_nothing();
    store_publish::test_a_build_holds_only_the_builder();
        // compose under a writer
    store_rebuild::test_a_writer_answers_during_a_rebuild();
    store_rebuild::test_a_write_during_a_rebuild_survives_it();
    store_rebuild::test_the_commit_publishes_the_filter_reasons();
    store_rebuild::test_concurrent_readers_compose_once();
    store_rebuild::test_collections_follow_their_components();
    store_rebuild::test_the_published_names_follow_the_set();
    store_rebuild::test_a_write_seeds_from_what_readers_hold();
        // one row moving
    store_patch::test_a_download_moves_its_row();
    store_patch::test_a_model_that_is_not_here();
    store_patch::test_a_collection_follows_its_component();
    store_patch::test_two_components_landing_together();
    store_patch::test_what_a_delete_takes_and_leaves();
    store_patch::test_options_land_on_the_live_row();

    if (g_failures != 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll store write checks passed\n");
    return 0;
}

}  // namespace write


namespace publication {

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::json;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// Each case performs one supported mutation, then asks a lock-free reader: what
// breaks if a mutating scope forgets to publish.
static std::string listing_name_of(const std::map<std::string, ModelInfo>& models,
                                   const std::string& repo) {
    for (const auto& [name, info] : models) {
        for (const auto& [type, checkpoint] : info.checkpoints) {
            if (checkpoint.find(repo) != std::string::npos) {
                return name;
            }
        }
    }
    return std::string();
}

static bool listing_is_self_consistent(const std::map<std::string, ModelInfo>& models) {
    for (const auto& [name, info] : models) {
        if (name != info.model_name) {
            return false;
        }
    }
    return true;
}

static void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// llamacpp checks the GGUF magic before it calls a file downloaded.
static fs::path make_hf_repo(const fs::path& models_dir, const std::string& repo,
                             const std::string& snapshot, const std::string& file) {
    std::string slug;
    for (char c : repo) {
        if (c == '/') slug += "--"; else slug += c;
    }
    fs::path repo_dir = models_dir / ("models--" + slug);
    write_file(repo_dir / "refs" / "main", snapshot);
    write_file(repo_dir / "snapshots" / snapshot / file, "GGUF0123456789");
    return repo_dir;
}

static void register_model(ModelManager& manager,
                           const std::string& name,
                           const std::string& recipe,
                           const std::string& checkpoint) {
    manager.register_user_model(name, json{{"model_name", name},
                                           {"recipe", recipe},
                                           {"checkpoint", checkpoint}});
}

static void test_registration_publishes(ModelManager& manager) {
    register_model(manager, "user.Published", "llamacpp", "zzorg/zzpublished:main.gguf");

    check("registering publishes the model to the existence check",
          manager.model_exists("user.Published"));
    check("registering publishes the model to the info lookup",
          manager.get_model_info("user.Published").recipe == "llamacpp");
    const std::string listed = listing_name_of(manager.get_supported_models(), "zzorg/zzpublished");
    check("registering publishes the model to the listing", !listed.empty());
    check("registering publishes a name that resolves back to the model",
          !listed.empty() && manager.get_model_info(listed).checkpoints.size() ==
                                 manager.get_model_info("user.Published").checkpoints.size());
    check("published listing keys match the names they carry",
          listing_is_self_consistent(manager.get_supported_models()));
}

static void test_deletion_publishes(ModelManager& manager) {
    manager.delete_model("user.Published");

    check("deleting publishes its absence from the existence check",
          !manager.model_exists("user.Published"));
    check("deleting publishes its absence from the listing",
          listing_name_of(manager.get_supported_models(), "zzorg/zzpublished").empty());
    bool threw = false;
    try {
        manager.get_model_info("user.Published");
    } catch (const std::exception&) {
        threw = true;
    }
    check("deleting publishes its absence from the info lookup", threw);
}

// Writing again through the same paths catches a store that publishes only once.
static void test_republish_after_replacement(ModelManager& manager) {
    register_model(manager, "user.Replaced", "llamacpp", "zzorg/zzfirst:main.gguf");
    check("the first definition is visible",
          manager.get_model_info("user.Replaced").checkpoints.count("main") == 1);

    manager.delete_model("user.Replaced");
    register_model(manager, "user.Replaced", "sd-cpp", "zzorg/zzsecond:vae.safetensors");

    const ModelInfo after = manager.get_model_info("user.Replaced");
    check("the replacement definition is visible", after.recipe == "sd-cpp");
    check("the previous definition is gone",
          listing_name_of(manager.get_supported_models(), "zzorg/zzfirst").empty());
    check("the listing stays self-consistent across replacement",
          listing_is_self_consistent(manager.get_supported_models()));
}

// Options are written into the cached entry in place with the cache still valid,
// so nothing rebuilds and publication is the only way the change can be seen.
static void test_in_place_update_publishes(ModelManager& manager) {
    register_model(manager, "user.Options", "llamacpp", "zzorg/zzoptions:main.gguf");

    // Chosen at run time so the case does not break when the option set changes.
    std::string key;
    for (const auto& candidate : lemon::RecipeOptions::keys_for_recipe("llamacpp")) {
        if (lemon::RecipeOptions("llamacpp", json{{candidate, json(4096)}}).get_option(candidate) ==
            json(4096)) {
            key = candidate;
            break;
        }
    }
    if (key.empty()) {
        check("llamacpp offers an option this test can set", false);
        return;
    }

    manager.replace_options("user.Options", json{{key, json(4096)}});

    check("an in-place update publishes to readers",
          manager.get_model_info("user.Options").recipe_options.get_option(key) == json(4096));
}

// Deciding a model is no longer complete rewrites the cached entry, and nothing
// rebuilds on its own, so publication is the only way the repair reaches readers.
static void test_repair_publishes(ModelManager& manager, const fs::path& models_dir) {
    const fs::path repo = make_hf_repo(models_dir, "zzorg/zzvanishing", "snap-x", "main.gguf");
    register_model(manager, "user.Vanishing", "llamacpp", "zzorg/zzvanishing:main.gguf");
    check("fixture starts out downloaded", manager.is_model_downloaded("user.Vanishing"));

    fs::remove(repo / "snapshots" / "snap-x" / "main.gguf");

    check("the missing file is reported", !manager.is_model_downloaded("user.Vanishing"));
    check("the repair publishes to readers",
          !manager.get_model_info("user.Vanishing").downloaded);
    check("the repaired model leaves the downloaded listing",
          listing_name_of(manager.get_downloaded_models(), "zzorg/zzvanishing").empty());
}

// A question about one row has to outlive the set that row lives in. `resolved_paths_for`
// used to borrow the row from a `read()` temporary, so a publish landing before the answer
// was finished left the loop reading memory the store had already freed - invisible to an
// assertion, fatal under a sanitizer.
static void test_one_row_read_outlives_the_snapshot() {
    lemon::models::AliasManager aliases{""};
    lemon::models::ModelStore store{aliases};
    const std::string id = "user.Raced";
    {
        lemon::models::ModelStore::Write write(store);
        ModelInfo& row = write.draft()[id];
        row.model_name = id;
        row.resolved_paths["main"] = "first.gguf";
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::atomic<long> answers{0};

    std::thread publisher([&] {
        while (!stop.load()) {
            lemon::models::ModelStore::Write write(store);
            write.draft()[id].resolved_paths["main"] = "second.gguf";
        }
    });

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const auto paths = store.resolved_paths_for(id);
                const auto found = paths.find("main");
                if (paths.size() != 1 || found == paths.end() ||
                    (found->second != "first.gguf" && found->second != "second.gguf")) {
                    torn.store(true);
                }
                const std::shared_ptr<const ModelInfo> row = store.find(id);
                if (!row || row->model_name != id || row->resolved_paths.size() != 1) {
                    torn.store(true);
                }
                ++answers;
            }
        });
    }

    for (int attempt = 0; attempt < 3000 && answers.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    publisher.join();
    for (auto& reader : readers) reader.join();

    check("one-row reads were answered throughout republication", answers.load() > 0);
    check("a one-row read never saw a torn answer", !torn.load());
}

// Readers run against the published snapshot while a writer keeps replacing it.
// Nothing is asserted mid-race: which generation a reader sees is timing.
static void test_republish_while_reading(ModelManager& manager) {
    manager.get_supported_models();

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::atomic<int> live{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            live.fetch_add(1);
            while (!stop.load()) {
                const std::size_t clouds = manager.count_cloud_models("zz-no-provider");
                const std::vector<std::string> hidden = manager.snapshot()->empty_recipes();
                (void)clouds;
                (void)hidden;
                reads.fetch_add(1);
            }
        });
    }

    const auto await = [](auto settled) {
        for (int attempt = 0; attempt < 5000 && !settled(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    await([&] { return live.load() == static_cast<int>(readers.size()); });

    for (int i = 0; i < 500; ++i) {
        manager.invalidate_models_cache();
    }

    await([&] { return reads.load() > 0; });
    stop.store(true);
    for (auto& reader : readers) {
        reader.join();
    }

    check("readers were served throughout republication", reads.load() > 0);
    check("listing stays self-consistent after concurrent republication",
          listing_is_self_consistent(manager.get_supported_models()));
}

int run() {
    ShowAllModels show_all_models;
    fs::path temp = fs::temp_directory_path();
    temp /= "store_publication_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(temp);
    lemon::utils::set_cache_dir(path_to_utf8(temp));
    lemon::utils::set_config_dir(path_to_utf8(temp));
    fs::path models_dir = temp / "hf";
    fs::create_directories(models_dir);
    lemon::utils::set_models_dir(path_to_utf8(models_dir));

    ModelManager manager(lemon::test::server_wired_config());
    test_registration_publishes(manager);
    test_deletion_publishes(manager);
    test_republish_after_replacement(manager);
    test_in_place_update_publishes(manager);
    test_repair_publishes(manager, models_dir);
    test_one_row_read_outlives_the_snapshot();
    test_republish_while_reading(manager);

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All store publication tests passed.\n");
    } else {
        std::printf("%d store publication test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace publication


namespace retry {

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

ModelInfo row(const std::string& name) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "llamacpp";
    info.checkpoints["main"] = "org/" + name + ":a.gguf";
    info.suggested = true;
    info.labels = {"chat"};
    return info;
}

bool answers(ModelManager& manager, const std::string& name) {
    try {
        return manager.get_model_info(name).recipe == "llamacpp";
    } catch (const std::exception&) {
        return false;
    }
}

// Stands in for the second pass of a compose that yielded: the registries now report a
// row that the first pass could not know about.
struct Late {
    ModelManager* manager = nullptr;
    std::atomic<int> passes{0};
};

}  // namespace

int run() {
    const fs::path scratch = lemon::test::make_scratch_dir("compose_retry");
    lemon::utils::set_models_dir((scratch / "models").string());

    Late late;
    auto racy = std::make_shared<lemon::models::EnumeratedCatalog>(
        lemon::models::CatalogDescriptor{"racy"},
        [&late](const lemon::models::CatalogAccumulator&) {
            if (++late.passes == 2 && late.manager != nullptr) {
                // Something persisted while this set was being put together. A writer
                // persists and invalidates; it never patches the composed set.
                late.manager->invalidate_models_cache();
                return std::vector<ModelInfo>{row("Fixture-Model")};
            }
            if (late.passes == 1) return std::vector<ModelInfo>{row("Fixture-Model")};
            return std::vector<ModelInfo>{row("Fixture-Model"), row("Late-Model")};
        });

    ModelManagerConfig config;
    config.cache_dir = scratch.string();
    config.extra_catalogs = {racy};
    // The compose protocol is the question, not what this machine can run.
    config.availability = [](const std::map<std::string, ModelInfo>&) {
        lemon::models::BackendAvailability declared;
        declared.system_ram_gb = 64.0;
        declared.largest_mem_pool_gb = 48.0;
        return declared;
    };
    ModelManager manager(std::move(config));
    late.manager = &manager;

    manager.get_supported_models();
    {
        manager.invalidate_models_cache();
    }

    const int composed_before = late.passes;
    manager.get_supported_models();
    const int composed_by_now = late.passes;
    check("the refused compose is asked again", composed_by_now >= composed_before + 2);
    // A read after the call composing again is the failure this guards: the caller of
    // the composing call is told the change landed, so it must already be there.
    check("and the change it yielded to is answered", answers(manager, "Late-Model"));
    check("by that call, not by one it provoked", late.passes == composed_by_now);
    check("the row it already had survives the retry", answers(manager, "Fixture-Model"));

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll compose-retry checks passed\n");
    return 0;
}

}  // namespace retry

int main() {
    int failures = 0;
    std::printf("\n=== write ===\n");
    failures |= write::run();
    std::printf("\n=== publication ===\n");
    failures |= publication::run();
    std::printf("\n=== retry ===\n");
    failures |= retry::run();
    return failures;
}
