// Filesystem-level tests for ModelManager::discover_extra_models() (#1667).
// The classifier-only tests in test_model_type_classifier.cpp cover
// infer_labels_from_name() and get_model_type_from_labels() in isolation;
// these tests exercise the real discovery pipeline (root files, split
// variants, and folder models) against a temp directory, so they also catch
// wiring bugs the isolated helper tests cannot see - such as a default
// deployment label ("chat") getting stamped before the filename is inspected,
// which used to make every discovered model resolve to ModelType::LLM
// regardless of what infer_labels_from_name() found.

#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelType;
using lemon::model_type_to_string;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "extra_model_discovery_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

// Discovery only reads filenames and file sizes, never GGUF content, so an
// empty placeholder file is a faithful fixture.
static void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path).close();
}

static const ModelInfo* find_model(const std::map<std::string, ModelInfo>& models,
                                   const std::string& id) {
    auto it = models.find(id);
    return it != models.end() ? &it->second : nullptr;
}

static bool has_label(const ModelInfo& info, const std::string& label) {
    return std::find(info.labels.begin(), info.labels.end(), label) != info.labels.end();
}

static bool check_type(const std::map<std::string, ModelInfo>& models,
                       const char* name,
                       const std::string& id,
                       ModelType expected,
                       const std::string& expected_label) {
    const ModelInfo* info = find_model(models, id);
    if (info == nullptr) {
        std::printf("[FAIL] %s (model '%s' not discovered)\n", name, id.c_str());
        ++g_failures;
        return false;
    }
    const bool ok = info->type == expected && has_label(*info, expected_label);
    std::printf("[%s] %s (type=%s, want=%s; has '%s' label=%s)\n",
                ok ? "PASS" : "FAIL", name,
                model_type_to_string(info->type).c_str(),
                model_type_to_string(expected).c_str(),
                expected_label.c_str(), has_label(*info, expected_label) ? "yes" : "no");
    if (!ok) ++g_failures;
    return ok;
}

// Root-level standalone files: covers model_manager.cpp's standalone_files loop.
static void test_root_files() {
    fs::path dir = make_temp_dir();
    touch(dir / "nomic-embed-text-v2.gguf");
    touch(dir / "bge-reranker-v2.gguf");
    touch(dir / "Qwen3-8B-Instruct.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models();

    check_type(models, "root embedding file -> EMBEDDING", "extra.nomic-embed-text-v2",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "root reranker file -> RERANKING", "extra.bge-reranker-v2",
               ModelType::RERANKING, "reranking");
    check_type(models, "root ordinary chat file stays LLM", "extra.Qwen3-8B-Instruct",
               ModelType::LLM, "chat");

    fs::remove_all(dir);
}

// Split-variant folder: multiple named quantizations of the same model,
// covers discover_extra_models_in_directory()'s split path.
static void test_split_variant_folder() {
    fs::path dir = make_temp_dir();
    touch(dir / "MyEmbedModel" / "MyEmbedModel-Q4_K_M.gguf");
    touch(dir / "MyEmbedModel" / "MyEmbedModel-Q8_0.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models();

    check_type(models, "split variant (Q4_K_M) -> EMBEDDING", "extra.MyEmbedModel-Q4_K_M",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "split variant (Q8_0) -> EMBEDDING", "extra.MyEmbedModel-Q8_0",
               ModelType::EMBEDDING, "embeddings");

    fs::remove_all(dir);
}

// Single-file folder model: covers discover_extra_models_in_directory()'s
// non-split (folder-as-one-model) path, where the folder name carries the
// signal and the file name does not.
static void test_folder_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "my-reranker-model" / "model.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models();

    check_type(models, "folder model -> RERANKING", "extra.my-reranker-model",
               ModelType::RERANKING, "reranking");

    fs::remove_all(dir);
}

// A model deploys in exactly one mode. When the folder name and the file name
// disagree, the file wins and only its label is applied - carrying both would
// be the label set illegal_deployment_labels() rejects.
static void test_folder_and_file_disagree() {
    fs::path dir = make_temp_dir();
    touch(dir / "embed-models" / "bge-reranker-v2.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models();

    if (check_type(models, "reranker in an embed-named folder -> RERANKING",
                   "extra.embed-models", ModelType::RERANKING, "reranking")) {
        const ModelInfo* info = find_model(models, "extra.embed-models");
        check("reranker in an embed-named folder is not also labeled embeddings",
              !has_label(*info, "embeddings"));
    }

    fs::remove_all(dir);
}

// An mmproj companion adds "vision" alongside the deployment label. vision is a
// capability, not a mode, so the model still deploys as an ordinary chat LLM.
static void test_multimodal_folder() {
    fs::path dir = make_temp_dir();
    touch(dir / "gemma-vision" / "gemma-vision-Q4_K_M.gguf");
    touch(dir / "gemma-vision" / "mmproj-gemma-vision-f16.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models();

    if (check_type(models, "multimodal folder stays LLM", "extra.gemma-vision",
                   ModelType::LLM, "chat")) {
        const ModelInfo* info = find_model(models, "extra.gemma-vision");
        check("multimodal folder gets the vision label", has_label(*info, "vision"));
        check("multimodal folder resolves an mmproj checkpoint",
              info->checkpoints.find("mmproj") != info->checkpoints.end());
    }

    fs::remove_all(dir);
}

int main() {
    // Redirect ModelManager's cache dir (server_models.json / user_models.json /
    // recipe_options.json) to a scratch location so construction never touches
    // the real user cache - the discovery logic under test doesn't use these
    // files, but the constructor loads them unconditionally.
    fs::path cache_dir = make_temp_dir();
    lemon::utils::set_cache_dir(cache_dir.string());

    test_root_files();
    test_split_variant_folder();
    test_folder_model();
    test_folder_and_file_disagree();
    test_multimodal_folder();

    fs::remove_all(cache_dir);

    if (g_failures == 0) {
        std::printf("All extra model discovery tests passed.\n");
    } else {
        std::printf("%d extra model discovery test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
