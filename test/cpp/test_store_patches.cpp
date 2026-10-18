// One row moving at a time: a download that landed, a model that went away, an option
// set that was re-layered. The store is real; nothing else is — no backend is asked
// about bytes, because none of these paths should be reaching for them.

#include <lemon/model_manager.h>
#include <lemon/model_types.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/store.h>
#include <lemon/models/store_snapshot.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

using lemon::ModelInfo;
using lemon::RecipeOptions;
using lemon::models::ArtifactSource;
using lemon::models::ModelArtifactStore;
using lemon::models::ModelStore;

static int g_failures = 0;

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
    ModelStore store;
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

int main() {
    test_a_download_moves_its_row();
    test_a_model_that_is_not_here();
    test_a_collection_follows_its_component();
    test_what_a_delete_takes_and_leaves();
    test_options_land_on_the_live_row();

    if (g_failures != 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll store patch checks passed\n");
    return 0;
}
