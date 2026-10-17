// A store lock covers the commit, not the computation. These tests stop a rebuild inside the
// work it used to hold the lock for and ask what the rest of the server can do meanwhile, what
// becomes of a write that lands in the window, and which rows a download landing patches.

#include <lemon/model_manager.h>
#include <lemon/model_types.h>
#include <lemon/models/catalog.h>
#include <lemon/models/store_builder.h>
#include <lemon/utils/path_utils.h>
#include <nlohmann/json.hpp>

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
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::RecipeOptions;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogDescriptor;
using lemon::models::EnumeratedCatalog;
using nlohmann::json;

static int g_failures = 0;

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
    manager.save_model_options(present);
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

int main() {
    test_a_writer_answers_during_a_rebuild();
    test_a_write_during_a_rebuild_survives_it();
    test_the_commit_publishes_the_filter_reasons();
    test_concurrent_readers_compose_once();
    test_collections_follow_their_components();
    test_the_published_names_follow_the_set();

    if (g_failures == 0) {
        std::printf("All store build tests passed.\n");
    } else {
        std::printf("%d store build test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
