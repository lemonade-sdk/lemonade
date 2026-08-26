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

namespace lemon {
struct ExtraModelDiscoveryTestHook {
    static std::map<std::string, ModelInfo> discover(const ModelManager& m) {
        return m.discover_extra_models();
    }
};
}  // namespace lemon

using lemon::ExtraModelDiscoveryTestHook;

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
    auto models = ExtraModelDiscoveryTestHook::discover(manager);

    check_type(models, "root embedding file -> EMBEDDING", "extra.nomic-embed-text-v2",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "root reranker file -> RERANKING", "extra.bge-reranker-v2",
               ModelType::RERANKING, "reranking");
    check_type(models, "root ordinary chat file stays LLM", "extra.Qwen3-8B-Instruct",
               ModelType::LLM, "chat");

    fs::remove_all(dir);
}

static void test_split_variant_folder() {
    fs::path dir = make_temp_dir();
    touch(dir / "MyEmbedModel" / "MyEmbedModel-Q4_K_M.gguf");
    touch(dir / "MyEmbedModel" / "MyEmbedModel-Q8_0.gguf");

    ModelManager manager(dir.string());
    auto models = ExtraModelDiscoveryTestHook::discover(manager);

    check_type(models, "split variant (Q4_K_M) -> EMBEDDING", "extra.MyEmbedModel-Q4_K_M",
               ModelType::EMBEDDING, "embeddings");
    check_type(models, "split variant (Q8_0) -> EMBEDDING", "extra.MyEmbedModel-Q8_0",
               ModelType::EMBEDDING, "embeddings");

    fs::remove_all(dir);
}

static void test_folder_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "my-reranker-model" / "model.gguf");

    ModelManager manager(dir.string());
    auto models = ExtraModelDiscoveryTestHook::discover(manager);

    check_type(models, "folder model -> RERANKING", "extra.my-reranker-model",
               ModelType::RERANKING, "reranking");

    fs::remove_all(dir);
}

// Carrying both labels is the set illegal_deployment_labels() rejects.
static void test_folder_and_file_disagree() {
    fs::path dir = make_temp_dir();
    touch(dir / "embed-models" / "bge-reranker-v2.gguf");

    ModelManager manager(dir.string());
    auto models = ExtraModelDiscoveryTestHook::discover(manager);

    if (check_type(models, "reranker in an embed-named folder -> RERANKING",
                   "extra.embed-models", ModelType::RERANKING, "reranking")) {
        const ModelInfo* info = find_model(models, "extra.embed-models");
        check("reranker in an embed-named folder is not also labeled embeddings",
              !has_label(*info, "embeddings"));
    }

    fs::remove_all(dir);
}

static void test_mixed_folder_is_one_model() {
    fs::path dir = make_temp_dir();
    touch(dir / "mixed" / "a-chat-model.gguf");
    touch(dir / "mixed" / "z-embed-model.gguf");

    ModelManager manager(dir.string());
    auto models = ExtraModelDiscoveryTestHook::discover(manager);

    check("mixed folder discovers one model", models.size() == 1);
    check_type(models, "mixed folder takes the primary file's mode", "extra.mixed",
               ModelType::LLM, "chat");

    fs::remove_all(dir);
}

// vision is a capability, not a mode, so the model still deploys as chat.
static void test_multimodal_folder() {
    fs::path dir = make_temp_dir();
    touch(dir / "gemma-vision" / "gemma-vision-Q4_K_M.gguf");
    touch(dir / "gemma-vision" / "mmproj-gemma-vision-f16.gguf");

    ModelManager manager(dir.string());
    auto models = ExtraModelDiscoveryTestHook::discover(manager);

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
    // The constructor loads the registry JSON files unconditionally, so point it
    // at a scratch dir to keep the test off the real user cache.
    fs::path cache_dir = make_temp_dir();
    lemon::utils::set_cache_dir(cache_dir.string());

    test_root_files();
    test_split_variant_folder();
    test_folder_model();
    test_folder_and_file_disagree();
    test_mixed_folder_is_one_model();
    test_multimodal_folder();

    fs::remove_all(cache_dir);

    if (g_failures == 0) {
        std::printf("All extra model discovery tests passed.\n");
    } else {
        std::printf("%d extra model discovery test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
