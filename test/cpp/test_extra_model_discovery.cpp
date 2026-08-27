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

// Discovery never reads GGUF content, so an empty file is a valid fixture.
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

static void test_root_files() {
    fs::path dir = make_temp_dir();
    touch(dir / "nomic-embed-text-v2.gguf");
    touch(dir / "bge-reranker-v2.gguf");
    touch(dir / "Qwen3-8B-Instruct.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check_type(models, "root embedding filename defaults to LLM", "extra.nomic-embed-text-v2",
               ModelType::LLM, "chat");
    check_type(models, "root reranker filename defaults to LLM", "extra.bge-reranker-v2",
               ModelType::LLM, "chat");
    check_type(models, "root ordinary chat file stays LLM", "extra.Qwen3-8B-Instruct",
               ModelType::LLM, "chat");

    fs::remove_all(dir);
}

static void test_split_variant_folder() {
    fs::path dir = make_temp_dir();
    touch(dir / "reranking" / "MyModel" / "MyModel-Q4_K_M.gguf");
    touch(dir / "reranking" / "MyModel" / "MyModel-Q8_0.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check_type(models, "split variant (Q4_K_M) inherits reranking", "extra.MyModel-Q4_K_M",
               ModelType::RERANKING, "reranking");
    check_type(models, "split variant (Q8_0) inherits reranking", "extra.MyModel-Q8_0",
               ModelType::RERANKING, "reranking");

    fs::remove_all(dir);
}

static void test_folder_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "my-model" / "model.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check_type(models, "folder model inherits embeddings", "extra.my-model",
               ModelType::EMBEDDING, "embeddings");

    fs::remove_all(dir);
}

static void test_filename_does_not_override_category() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "bge-reranker-v2.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    if (check_type(models, "reranker filename in embeddings stays EMBEDDING",
                   "extra.bge-reranker-v2", ModelType::EMBEDDING, "embeddings")) {
        const ModelInfo* info = find_model(models, "extra.bge-reranker-v2");
        check("filename does not add reranking", !has_label(*info, "reranking"));
    }

    fs::remove_all(dir);
}

static void test_mixed_folder_is_one_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "mixed" / "a-embed-model.gguf");
    touch(dir / "mixed" / "z-rerank-model.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check("mixed folder discovers one model", models.size() == 1);
    check_type(models, "ordinary folder defaults to chat", "extra.mixed",
               ModelType::LLM, "chat");

    fs::remove_all(dir);
}

// vision is a capability, not a mode, so the model still deploys as chat.
static void test_multimodal_folder() {
    fs::path dir = make_temp_dir();
    touch(dir / "chat" / "gemma-vision" / "gemma-vision-Q4_K_M.gguf");
    touch(dir / "chat" / "gemma-vision" / "mmproj-gemma-vision-f16.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    if (check_type(models, "multimodal folder stays LLM", "extra.gemma-vision",
                   ModelType::LLM, "chat")) {
        const ModelInfo* info = find_model(models, "extra.gemma-vision");
        check("multimodal folder gets the vision label", has_label(*info, "vision"));
        check("multimodal folder resolves an mmproj checkpoint",
              info->checkpoints.find("mmproj") != info->checkpoints.end());
    }

    fs::remove_all(dir);
}

static void test_category_files_are_separate_models() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "model-a.gguf");
    touch(dir / "embeddings" / "model-b.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check("category files discover two models", models.size() == 2);
    check_type(models, "first category file is embedding", "extra.model-a",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "second category file is embedding", "extra.model-b",
               ModelType::EMBEDDING, "embeddings");

    fs::remove_all(dir);
}

static void test_category_names_are_case_sensitive() {
    fs::path dir = make_temp_dir();
    touch(dir / "Embeddings" / "model.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check_type(models, "unreserved mixed-case directory defaults to chat", "extra.Embeddings",
               ModelType::LLM, "chat");

    fs::remove_all(dir);
}

static void test_same_name_across_categories() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "model.gguf");
    touch(dir / "reranking" / "model.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    int embedding_models = 0;
    int reranking_models = 0;
    for (const auto& [id, info] : models) {
        (void)id;
        if (info.type == ModelType::EMBEDDING && has_label(info, "embeddings")) {
            ++embedding_models;
        }
        if (info.type == ModelType::RERANKING && has_label(info, "reranking")) {
            ++reranking_models;
        }
    }

    check("same-named category files are both discovered", models.size() == 2);
    check("same-named category files keep one embedding", embedding_models == 1);
    check("same-named category files keep one reranker", reranking_models == 1);

    fs::remove_all(dir);
}

static void test_non_normalized_search_path() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "model-a.gguf");
    touch(dir / "embeddings" / "model-b.gguf");

    ModelManager manager((dir / ".").string());
    auto models = manager.discover_extra_models_for_test();

    check("non-normalized category path discovers two models", models.size() == 2);
    check_type(models, "first non-normalized path model is embedding", "extra.model-a",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "second non-normalized path model is embedding", "extra.model-b",
               ModelType::EMBEDDING, "embeddings");

    fs::remove_all(dir);
}

int main() {
    // The constructor loads the registry JSON files unconditionally, so point it
    // at a scratch dir to keep the test off the real user cache.
    fs::path cache_dir = make_temp_dir();
    lemon::utils::set_cache_dir(cache_dir.string());

    test_root_files();
    test_split_variant_folder();
    test_folder_model();
    test_filename_does_not_override_category();
    test_mixed_folder_is_one_model();
    test_multimodal_folder();
    test_category_files_are_separate_models();
    test_category_names_are_case_sensitive();
    test_same_name_across_categories();
    test_non_normalized_search_path();

    fs::remove_all(cache_dir);

    if (g_failures == 0) {
        std::printf("All extra model discovery tests passed.\n");
    } else {
        std::printf("%d extra model discovery test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
