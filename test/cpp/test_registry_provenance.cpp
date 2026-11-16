// The provenance file: what v1 readers may keep reading, what a v2 write adds,
// and which sentence a shared auxiliary repository is allowed to tell.

#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/model_info.h>

#include "support/test_state_isolation.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::models::ModelArtifactStore;
using lemon::models::Provenance;

namespace {

std::vector<std::string> g_failures;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << std::endl;
        g_failures.push_back(what);
    }
}

ModelArtifactStore make_store() {
    ModelArtifactStore::Hooks hooks;
    hooks.cache_root = [] { return lemon::test::make_scratch_dir("prov").string(); };
    hooks.source_for = [](const ModelInfo&, const std::string&) { return nullptr; };
    return ModelArtifactStore(hooks);
}

ModelInfo two_checkpoint_model() {
    ModelInfo info;
    info.model_name = "user.Two-Part";
    info.recipe = "llamacpp";
    info.checkpoints["main"] = "vendor/repo:model.gguf";
    info.checkpoints["side"] = "vendor/aux:side.bin";
    return info;
}

Provenance provenance(const std::string& repo_id, const std::string& snapshot) {
    return Provenance{"huggingface", repo_id, "main", snapshot, snapshot};
}

nlohmann::json read(const fs::path& dir) {
    std::ifstream in(dir / ".lemonade_registry.json", std::ios::binary);
    return nlohmann::json::parse(in);
}

// v1 said one thing, in one shape, and shipped readers stop at that shape.
void test_a_v1_file_is_read_forever() {
    ModelArtifactStore store = make_store();
    const fs::path dir = lemon::test::make_scratch_dir("v1repo");
    const ModelInfo info = two_checkpoint_model();

    // A v1 file by hand: entry.selection is the whole recipe-plus-checkpoints
    // sentence the v1 writer wrote, entry.snapshot_id the active one.
    const std::string v1_selection =
        std::string("llamacpp\nmain=vendor/repo:model.gguf\nside=vendor/aux:side.bin");
    nlohmann::json v1;
    v1["source"] = "huggingface";
    v1["repo_id"] = "vendor/repo";
    v1["revision"] = "main";
    v1["snapshot_id"] = "aaaa1111";
    v1["processed_models"]["user.Two-Part"] = {{"selection", v1_selection},
                                               {"snapshot_id", "aaaa1111"}};
    std::ofstream(dir / ".lemonade_registry.json") << v1.dump();

    check(store.processed_snapshot_id(dir, info) == "aaaa1111",
          "a v1 file answers a v2-era reader exactly as it answered its own");
}

void test_selection_mismatch_clears_the_fast_path() {
    ModelArtifactStore store = make_store();
    const fs::path dir = lemon::test::make_scratch_dir("mismatch");
    const ModelInfo info = two_checkpoint_model();

    // Write v1 through the store's own v2 writer with legacy contents: use a
    // main record, then re-record under a changed selection.
    store.record_processed_model(dir, info, provenance("vendor/repo", "aaaa1111"));
    const std::string hit = store.processed_snapshot_id(dir, info);
    check(!hit.empty(), "a matching selection answers with its snapshot");

    ModelInfo changed = info;
    changed.checkpoints["main"] = "vendor/repo:other.gguf";
    check(store.processed_snapshot_id(dir, changed).empty(),
          "a changed selection clears the fast path, so the pull happens");
}

void test_a_v2_write_records_each_checkpoint_where_it_lands() {
    ModelArtifactStore store = make_store();
    const fs::path main_dir = lemon::test::make_scratch_dir("v2main");
    const fs::path aux_dir = lemon::test::make_scratch_dir("v2aux");
    const ModelInfo info = two_checkpoint_model();

    store.record_processed_model(main_dir, info, provenance("vendor/repo", "snap-main-1"), "main");
    store.record_processed_model(aux_dir, info, provenance("vendor/aux", "snap-aux-1"), "side");

    const nlohmann::json main_file = read(main_dir);
    const auto& main_entry = main_file["processed_models"]["user.Two-Part"];
    check(main_entry.contains("selection"), "the main file still tells the v1 sentence");
    check(main_entry.value("snapshot_id", "") == "snap-main-1",
          "the v2 main entry still answers a reader that only knows v1");
    check(main_entry["checkpoints"].contains("main"), "the main file names its checkpoint");
    check(!main_entry["checkpoints"].contains("side"),
          "the main file says nothing about bytes it never held");
    check(main_entry["checkpoints"]["main"]["snapshot_id"] == "snap-main-1",
          "the main checkpoint records the main snapshot");

    const nlohmann::json aux_file = read(aux_dir);
    check(aux_file.value("version", 0) == 2, "a checkpointed write announces v2");
    const auto& aux_entry = aux_file["processed_models"]["user.Two-Part"];
    check(!aux_entry.contains("selection"),
          "an auxiliary file does not claim the whole model");
    check(aux_entry["checkpoints"].contains("side") &&
              !aux_entry["checkpoints"].contains("main"),
          "the auxiliary file records only its own checkpoint");
    check(aux_entry["checkpoints"]["side"]["snapshot_id"] == "snap-aux-1",
          "the side checkpoint records the auxiliary snapshot");
    // A v1 reader that stops at the top level still reads sense.
    check(aux_file.value("repo_id", "") == "vendor/aux" &&
              aux_file["snapshot_id"] == "snap-aux-1" &&
              aux_file["processed_models"].contains("user.Two-Part"),
          "the top level stays readable by a reader that never opens checkpoints");
}

void test_two_models_share_an_auxiliary_file_without_quarrelling() {
    ModelArtifactStore store = make_store();
    const fs::path aux_dir = lemon::test::make_scratch_dir("shared-aux");

    ModelInfo first = two_checkpoint_model();
    ModelInfo second = first;
    second.model_name = "user.Other-Part";
    second.checkpoints["side"] = "vendor/aux:side.bin";

    store.record_processed_model(aux_dir, first, provenance("vendor/aux", "snap-aux-1"), "side");
    store.record_processed_model(aux_dir, second, provenance("vendor/aux", "snap-aux-1"), "side");

    const nlohmann::json file = read(aux_dir);
    check(file["processed_models"].contains("user.Two-Part") &&
              file["processed_models"].contains("user.Other-Part"),
          "both dependents survive in the shared file");
}

void test_a_moved_main_snapshot_keeps_the_one_it_left_named() {
    ModelArtifactStore store = make_store();
    const fs::path dir = lemon::test::make_scratch_dir("moved");
    const ModelInfo info = two_checkpoint_model();

    store.record_processed_model(dir, info, provenance("vendor/repo", "snap-old"));
    store.record_processed_model(dir, info, provenance("vendor/repo", "snap-new"));

    const nlohmann::json file = read(dir);
    const auto& entry = file["processed_models"]["user.Two-Part"];
    check(entry.value("previous_snapshot_id", "") == "snap-old",
          "the superseded snapshot stays named");
    check(entry["checkpoints"]["main"]["snapshot_id"] == "snap-new",
          "the checkpoint slot moves to the new snapshot");
}

}  // namespace

int main() {
    lemon::test::state_dir();
    test_a_v1_file_is_read_forever();
    test_selection_mismatch_clears_the_fast_path();
    test_a_v2_write_records_each_checkpoint_where_it_lands();
    test_two_models_share_an_auxiliary_file_without_quarrelling();
    test_a_moved_main_snapshot_keeps_the_one_it_left_named();

    if (!g_failures.empty()) {
        std::cerr << g_failures.size() << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "all registry-provenance checks passed" << std::endl;
    return 0;
}
