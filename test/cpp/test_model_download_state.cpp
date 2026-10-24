#include "lemon/model_manager.h"
#include "lemon/models/artifact_source.h"
#include "lemon/models/artifact_store.h"
#include "lemon/models/store_snapshot.h"
#include "lemon/utils/path_utils.h"
#include "support/test_state_isolation.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "show_all_models.h"

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::json;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// Completeness is a question about a row and the bytes on disk, so the row never has to be
// registered: this store is wired like the manager's, minus the two hooks the question does
// not use.
static lemon::models::ModelArtifactStore artifacts_for_test() {
    return lemon::models::ModelArtifactStore{lemon::models::ModelArtifactStore::Hooks{
        [] { return std::string(); },
        [](const ModelInfo&, const std::string&)
            -> std::unique_ptr<lemon::models::ArtifactSource> { return nullptr; },
        [] { return std::string(); },
    }};
}

static void write_file(const fs::path& path, const std::string& contents = "data") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

struct RegistryFixtures {
    fs::path hf_variant_snapshot;
    fs::path hf_uncommitted_repo;
    fs::path hf_uncommitted_snapshot;
    fs::path collection_main;
    fs::path collection_vae;
};

static RegistryFixtures create_registry_fixtures(const fs::path& root,
                                                  const fs::path& hf_root) {
    RegistryFixtures fixtures;

    fs::path hf_variant_repo = hf_root / "models--org--variant";
    fixtures.hf_variant_snapshot = hf_variant_repo / "snapshots" / "snapshot-one";
    write_file(fixtures.hf_variant_snapshot / "model.gguf", "GGUF");
    write_file(hf_variant_repo / "refs" / "main", "snapshot-one");

    fixtures.hf_uncommitted_repo = hf_root / "models--org--uncommitted";
    fixtures.hf_uncommitted_snapshot =
        fixtures.hf_uncommitted_repo / "snapshots" / "incomplete-snapshot";
    write_file(fixtures.hf_uncommitted_snapshot / ".download_manifest.json", "{}");
    write_file(
        fixtures.hf_uncommitted_snapshot / "model-00002-of-00004.safetensors.partial",
        "partial");

    fs::path collection_dir = root / "collection";
    fixtures.collection_main = collection_dir / "main.bin";
    fixtures.collection_vae = collection_dir / "vae.bin";
    write_file(fixtures.collection_main);

    json user_models = {
        // A host spelled out: a manager with no default model source cannot build a
        // repository for a bare "org/repo", and checkpoint_status reports nothing.
        {"hf-variant",
         json{{"checkpoint", "org/variant:model.gguf"}, {"recipe", "llamacpp"},
              {"source", "huggingface"}}},
        {"hf-uncommitted",
         json{{"checkpoint", "org/uncommitted"}, {"recipe", "sd-cpp"},
              {"source", "huggingface"}}},
        {"comp-multi",
         json{{"checkpoints",
               json{{"main", path_to_utf8(fixtures.collection_main)},
                    {"vae", path_to_utf8(fixtures.collection_vae)}}},
              {"recipe", "llamacpp"},
              {"source", "local_path"}}},
        {"coll-test",
         json{{"components", json::array({"user.comp-multi"})},
              {"recipe", "collection.omni"}}},
    };
    write_file(root / "user_models.json", user_models.dump(2));
    return fixtures;
}

// One answer, asked two ways: `downloaded` from the store's completeness rule,
// checkpoint_status[].present from the source that would fetch the bytes. Both are pinned to
// the answer the bytes deserve, not merely to each other — a build that forgot the in-flight
// markers makes the two agree on "present" and would pass an agreement-only check.
static void check_one_answer(ModelManager& manager, const std::string& model, bool expected,
                             const char* what) {
    const ModelInfo info = manager.get_model_info(model);
    const auto statuses = manager.checkpoint_status(info);
    bool required_all_present = true;
    for (const auto& status : statuses) {
        if (status.required && !status.present) required_all_present = false;
    }
    const std::string name = std::string(what) + ": the store says";
    check(name.c_str(), info.downloaded == expected);
    const std::string pair = std::string(what) + ": checkpoint_status says the same";
    check(pair.c_str(), required_all_present == expected);
}

static void test_one_presence_answer(ModelManager& manager, const RegistryFixtures& fixtures) {
    const fs::path variant_file = fixtures.hf_variant_snapshot / "model.gguf";

    write_file(fixtures.hf_variant_snapshot / ".download_manifest.json", "{}");
    manager.invalidate_models_cache();
    check_one_answer(manager, "user.hf-variant", false,
                     "a manifest beside a resolved checkpoint");
    fs::remove(fixtures.hf_variant_snapshot / ".download_manifest.json");

    write_file(path_from_utf8(path_to_utf8(variant_file) + ".partial"), "partial");
    manager.invalidate_models_cache();
    check_one_answer(manager, "user.hf-variant", false,
                     "a .partial beside a resolved checkpoint");
    fs::remove(path_from_utf8(path_to_utf8(variant_file) + ".partial"));

    write_file(fixtures.hf_uncommitted_repo / "refs" / "main", "incomplete-snapshot");
    manager.invalidate_models_cache();
    check_one_answer(manager, "user.hf-uncommitted", false,
                     "a .partial inside a directory checkpoint");
    fs::remove(fixtures.hf_uncommitted_repo / "refs" / "main");

    write_file(variant_file, "<html><body>Rate limit exceeded</body></html>");
    manager.invalidate_models_cache();
    check_one_answer(manager, "user.hf-variant", false, "an HTML error page named .gguf");
    write_file(variant_file, "GGUF");

    // The other direction: agreement must not be bought by answering false everywhere.
    manager.invalidate_models_cache();
    check_one_answer(manager, "user.comp-multi", false, "a component missing one checkpoint");
    write_file(fixtures.collection_vae);
    manager.invalidate_models_cache();
    check_one_answer(manager, "user.comp-multi", true, "a component holding both checkpoints");
    check_one_answer(manager, "user.hf-variant", true, "a committed registry checkpoint");

    // The component test below starts from a component missing its second file.
    fs::remove(fixtures.collection_vae);
    manager.invalidate_models_cache();
}

static void test_required_checkpoint_completeness(ModelManager& manager,
                                                  const fs::path& root) {
    fs::path dir = root / "required-checkpoints";
    fs::path main_path = dir / "main.bin";
    fs::path vae_path = dir / "vae.bin";

    ModelInfo info;
    info.recipe = "llamacpp";
    // Presence is asked of the source that would fetch the bytes, so the row has to say
    // where they came from; a checkpoint name alone binds to nothing.
    info.source = "local_path";
    info.checkpoints = {{"main", "main"}, {"vae", "vae"}};
    info.resolved_paths = {
        {"main", path_to_utf8(main_path)},
        {"vae", path_to_utf8(vae_path)},
    };

    check("multi-checkpoint: no files is incomplete",
          !artifacts_for_test().complete(info));

    write_file(main_path);
    check("multi-checkpoint: missing auxiliary file is incomplete",
          !artifacts_for_test().complete(info));

    write_file(vae_path);
    check("multi-checkpoint: all required files is complete",
          artifacts_for_test().complete(info));

    fs::path partial_path = main_path;
    partial_path += ".partial";
    write_file(partial_path, "partial");
    check("multi-checkpoint: .partial makes checkpoint incomplete",
          !artifacts_for_test().complete(info));

    fs::remove(partial_path);
    check("multi-checkpoint: removing .partial restores completeness",
          artifacts_for_test().complete(info));

    fs::path manifest = dir / ".download_manifest.json";
    write_file(manifest, "{}");
    check("multi-checkpoint: download manifest marks interrupted download",
          !artifacts_for_test().complete(info));

    fs::remove(manifest);
    check("multi-checkpoint: removing manifest restores completeness",
          artifacts_for_test().complete(info));
}

// A side cache the engine fetches on first use must not hold the model back: the
// whisper NPU build pulls the .rai beside the .bin the first time it needs one.
static void test_a_side_cache_is_not_required(ModelManager& manager, const fs::path& root) {
    const fs::path dir = root / "side-cache";
    const fs::path main_path = dir / "model.bin";
    const fs::path side_path = dir / "model.bin.rai";

    ModelInfo info;
    info.recipe = "whispercpp";
    info.source = "local_path";
    info.checkpoints = {{"main", "main"}, {"npu_cache", "npu_cache"}};
    info.resolved_paths = {
        {"main", path_to_utf8(main_path)},
        {"npu_cache", path_to_utf8(side_path)},
    };

    check("with neither file present the model is not downloaded",
          !artifacts_for_test().complete(info));

    write_file(main_path);
    check("the model is downloaded without its side cache",
          artifacts_for_test().complete(info));

    write_file(side_path);
    check("and stays downloaded once the side cache arrives",
          artifacts_for_test().complete(info));

    // The optionality is the backend's declaration, not a property of the name.
    info.recipe = "llamacpp";
    fs::remove(side_path);
    check("a backend that fetches everything needs all of it",
          !artifacts_for_test().complete(info));
}

static void test_single_checkpoint_partial_guard(ModelManager& manager,
                                                 const fs::path& root) {
    fs::path model_path = root / "single-checkpoint" / "model.bin";
    write_file(model_path);

    ModelInfo info;
    info.recipe = "llamacpp";
    info.source = "local_path";
    info.checkpoints = {{"main", "main"}};
    info.resolved_paths = {{"main", path_to_utf8(model_path)}};

    check("single checkpoint remains complete without marker",
          artifacts_for_test().complete(info));

    fs::path partial_path = model_path;
    partial_path += ".partial";
    write_file(partial_path, "partial");
    check("single checkpoint .partial guard remains active",
          !artifacts_for_test().complete(info));
}

static void test_hf_manifest_marks_variant_incomplete(
    ModelManager& manager, const RegistryFixtures& fixtures) {
    check("HF variant: committed file is downloaded",
          manager.get_model_info("user.hf-variant").downloaded);

    write_file(fixtures.hf_variant_snapshot / ".download_manifest.json", "{}");
    manager.invalidate_models_cache();
    check("HF variant: manifest marks interrupted snapshot incomplete",
          !manager.get_model_info("user.hf-variant").downloaded);
}

static void test_variantless_snapshot_commit_state(
    ModelManager& manager, const RegistryFixtures& fixtures) {
    check("variantless HF: uncommitted snapshot is incomplete",
          !manager.get_model_info("user.hf-uncommitted").downloaded);

    fs::remove(fixtures.hf_uncommitted_snapshot / ".download_manifest.json");
    fs::remove(
        fixtures.hf_uncommitted_snapshot / "model-00002-of-00004.safetensors.partial");
    write_file(fixtures.hf_uncommitted_snapshot / "config.json", "{}");
    write_file(fixtures.hf_uncommitted_repo / "refs" / "main", "incomplete-snapshot");
    manager.invalidate_models_cache();

    ModelInfo committed = manager.get_model_info("user.hf-uncommitted");
    check("variantless HF: committed snapshot is downloaded",
          committed.downloaded &&
          path_from_utf8(committed.resolved_path()) == fixtures.hf_uncommitted_snapshot);

    fs::path interrupted_update =
        fixtures.hf_uncommitted_repo / "snapshots" / "new-incomplete-snapshot";
    write_file(interrupted_update / ".download_manifest.json", "{}");
    write_file(interrupted_update / "model.safetensors.partial", "partial");
    manager.invalidate_models_cache();

    ModelInfo updating = manager.get_model_info("user.hf-uncommitted");
    check("variantless HF: interrupted update keeps committed snapshot usable",
          updating.downloaded &&
          path_from_utf8(updating.resolved_path()) == fixtures.hf_uncommitted_snapshot);
}

static void test_collection_component_download_state(
    ModelManager& manager, const RegistryFixtures& fixtures) {
    check("collection: incomplete component is not downloaded",
          !manager.get_model_info("user.comp-multi").downloaded);
    check("collection: incomplete component keeps collection incomplete",
          !manager.get_model_info("user.coll-test").downloaded);

    write_file(fixtures.collection_vae);
    manager.invalidate_models_cache();
    check("collection: complete component is downloaded",
          manager.get_model_info("user.comp-multi").downloaded);
    check("collection: all complete components make collection downloaded",
          manager.get_model_info("user.coll-test").downloaded);

    fs::path partial_path = fixtures.collection_main;
    partial_path += ".partial";
    write_file(partial_path, "partial");
    manager.invalidate_models_cache();
    check("collection: component .partial makes component incomplete again",
          !manager.get_model_info("user.comp-multi").downloaded);
    check("collection: component .partial propagates to collection status",
          !manager.get_model_info("user.coll-test").downloaded);
}

int main() {
    ShowAllModels show_all_models;
    const fs::path temp = lemon::test::state_dir();
    fs::path hf_root = temp / "hf";
    fs::create_directories(hf_root);

    lemon::utils::set_models_dir(path_to_utf8(hf_root));

    RegistryFixtures fixtures = create_registry_fixtures(temp, hf_root);

    {
        ModelManager manager;
        test_one_presence_answer(manager, fixtures);
        test_required_checkpoint_completeness(manager, temp);
        test_a_side_cache_is_not_required(manager, temp);
        test_single_checkpoint_partial_guard(manager, temp);
        test_hf_manifest_marks_variant_incomplete(manager, fixtures);
        test_variantless_snapshot_commit_state(manager, fixtures);
        test_collection_component_download_state(manager, fixtures);
    }

    if (g_failures == 0) {
        std::printf("All model download-state tests passed.\n");
    } else {
        std::printf("%d model download-state test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
