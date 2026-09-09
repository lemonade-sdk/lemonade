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

static bool has_alias(const ModelInfo& info, const std::string& alias) {
    return std::find(info.input_aliases.begin(), info.input_aliases.end(), alias) !=
           info.input_aliases.end();
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

static std::map<std::string, std::string> discovery_signature(
    const std::map<std::string, ModelInfo>& models) {
    std::map<std::string, std::string> signature;
    for (const auto& [id, info] : models) {
        std::vector<std::string> labels = info.labels;
        std::vector<std::string> aliases = info.input_aliases;
        std::sort(labels.begin(), labels.end());
        std::sort(aliases.begin(), aliases.end());

        std::string value = model_type_to_string(info.type);
        for (const auto& label : labels) value += "|label:" + label;
        for (const auto& alias : aliases) value += "|alias:" + alias;
        for (const auto& [kind, path] : info.resolved_paths) {
            value += "|path:" + kind + "=" + fs::path(path).filename().string();
        }
        signature.emplace(id, std::move(value));
    }
    return signature;
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

static void test_direct_multimodal_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "chat" / "gemma.gguf");
    touch(dir / "chat" / "mmproj-gemma.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    const ModelInfo* info = find_model(models, "extra.gemma");
    check("direct multimodal model is discovered alone", models.size() == 1 && info != nullptr);
    if (info != nullptr) {
        check("direct multimodal model stays chat",
              info->type == ModelType::LLM && has_label(*info, "chat"));
        check("direct multimodal model gets vision", has_label(*info, "vision"));
        check("direct multimodal model resolves its mmproj",
              info->resolved_paths.find("mmproj") != info->resolved_paths.end() &&
              info->resolved_paths.at("mmproj") ==
                  (dir / "chat" / "mmproj-gemma.gguf").string());
    }

    fs::remove_all(dir);
}

static void test_ambiguous_direct_mmproj_is_not_attached() {
    fs::path dir = make_temp_dir();
    touch(dir / "chat" / "gemma.gguf");
    touch(dir / "chat" / "llama.gguf");
    touch(dir / "chat" / "mmproj.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check("ambiguous direct models stay separate", models.size() == 2);
    for (const auto& model : models) {
        const ModelInfo& info = model.second;
        check("ambiguous direct mmproj is not guessed",
              info.checkpoints.find("mmproj") == info.checkpoints.end() &&
              !has_label(info, "vision"));
    }

    fs::remove_all(dir);
}

static void test_direct_shards_with_independent_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "chat" / "large-Q4_K_M-00001-of-00002.gguf");
    touch(dir / "chat" / "large-Q4_K_M-00002-of-00002.gguf");
    touch(dir / "chat" / "small.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check("direct shard set and independent model produce two models", models.size() == 2);
    check_type(models, "direct shards are grouped", "extra.large-Q4_K_M",
               ModelType::LLM, "chat");
    check_type(models, "independent direct model stays separate", "extra.small",
               ModelType::LLM, "chat");

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

    check("same-named category files are both discovered", models.size() == 2);
    check_type(models, "embedding duplicate gets deterministic short id", "extra.model",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "reranking duplicate gets deterministic qualified id",
               "extra.reranking-model", ModelType::RERANKING, "reranking");

    fs::remove_all(dir);
}

static void test_reserved_directory_keeps_folder_id() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "all-MiniLM-L6-v2.gguf");
    touch(dir / "embeddings" / "nomic-embed-text-v2.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    const ModelInfo* first = find_model(models, "extra.all-MiniLM-L6-v2");
    const ModelInfo* second = find_model(models, "extra.nomic-embed-text-v2");
    if (first == nullptr || second == nullptr) {
        check("reserved directory files are listed individually", false);
    } else {
        check("folder id resolves to the first file",
              has_alias(*first, "extra.embeddings") && has_alias(*first, "embeddings"));
        check("folder id is not duplicated across files",
              !has_alias(*second, "extra.embeddings"));
    }

    fs::remove_all(dir);
}

static void test_reserved_directory_model_cannot_claim_folder_id() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "a.gguf");
    touch(dir / "embeddings" / "embeddings.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    const ModelInfo* first = find_model(models, "extra.a");
    const ModelInfo* colliding = find_model(models, "extra.embeddings-embeddings");
    check("reserved directory collision discovers both models", models.size() == 2);
    check("first model keeps the reserved directory aliases",
          first != nullptr && has_alias(*first, "embeddings") &&
          has_alias(*first, "extra.embeddings"));
    check("colliding model gets a qualified id without the reserved aliases",
          colliding != nullptr && !has_alias(*colliding, "embeddings") &&
          !has_alias(*colliding, "extra.embeddings"));

    fs::remove_all(dir);
}

static void test_category_model_cannot_claim_another_folder_id() {
    fs::path dir = make_temp_dir();
    touch(dir / "chat" / "embeddings.gguf");
    touch(dir / "embeddings" / "a.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    const ModelInfo* embedding = find_model(models, "extra.a");
    check("cross-category reserved id collision discovers both models", models.size() == 2);
    check_type(models, "cross-category collision gets a qualified id",
               "extra.chat-embeddings", ModelType::LLM, "chat");
    check("other category keeps its reserved directory alias",
          embedding != nullptr && has_alias(*embedding, "extra.embeddings"));

    fs::remove_all(dir);
}

static void test_nested_category_model_cannot_claim_folder_id() {
    fs::path dir = make_temp_dir();
    touch(dir / "embeddings" / "a.gguf");
    touch(dir / "embeddings" / "embeddings" / "model.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    const ModelInfo* direct = find_model(models, "extra.a");
    check("nested reserved id collision discovers both models", models.size() == 2);
    check_type(models, "nested reserved id collision gets a qualified id",
               "extra.embeddings-embeddings", ModelType::EMBEDDING, "embeddings");
    check("direct model keeps the reserved directory alias",
          direct != nullptr && has_alias(*direct, "extra.embeddings"));

    fs::remove_all(dir);
}

static void test_root_beats_category_for_short_id() {
    fs::path dir = make_temp_dir();
    touch(dir / "nomic-embed-text-v2.gguf");
    touch(dir / "embeddings" / "nomic-embed-text-v2.gguf");

    ModelManager manager(dir.string());
    auto models = manager.discover_extra_models_for_test();

    check("root and category duplicates are both discovered", models.size() == 2);
    check_type(models, "root file keeps the short id", "extra.nomic-embed-text-v2",
               ModelType::LLM, "chat");
    check_type(models, "category duplicate gets the qualified id",
               "extra.embeddings-nomic-embed-text-v2", ModelType::EMBEDDING, "embeddings");

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

static void test_discovery_is_independent_of_creation_order() {
    const std::vector<fs::path> files = {
        "root.gguf",
        fs::path("chat") / "vision" / "model.gguf",
        fs::path("chat") / "vision" / "mmproj-model.gguf",
        fs::path("embeddings") / "duplicate.gguf",
        fs::path("embeddings") / "nested" / "nested-Q8_0.gguf",
        fs::path("embeddings") / "nested" / "nested-Q4_K_M.gguf",
        fs::path("reranking") / "duplicate.gguf",
    };

    auto discover_in_order = [&](std::vector<fs::path> ordered_files) {
        fs::path dir = make_temp_dir();
        for (const auto& file : ordered_files) touch(dir / file);
        ModelManager manager(dir.string());
        auto signature = discovery_signature(manager.discover_extra_models_for_test());
        fs::remove_all(dir);
        return signature;
    };

    const auto forward = discover_in_order(files);
    auto reversed_files = files;
    std::reverse(reversed_files.begin(), reversed_files.end());
    const auto reversed = discover_in_order(reversed_files);

    check("discovery result is independent of file creation order", forward == reversed);
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
    test_direct_multimodal_model();
    test_ambiguous_direct_mmproj_is_not_attached();
    test_direct_shards_with_independent_model();
    test_category_files_are_separate_models();
    test_category_names_are_case_sensitive();
    test_same_name_across_categories();
    test_reserved_directory_keeps_folder_id();
    test_reserved_directory_model_cannot_claim_folder_id();
    test_category_model_cannot_claim_another_folder_id();
    test_nested_category_model_cannot_claim_folder_id();
    test_root_beats_category_for_short_id();
    test_non_normalized_search_path();
    test_discovery_is_independent_of_creation_order();

    fs::remove_all(cache_dir);

    if (g_failures == 0) {
        std::printf("All extra model discovery tests passed.\n");
    } else {
        std::printf("%d extra model discovery test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
