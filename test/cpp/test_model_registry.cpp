#include "lemon/model_manager.h"
#include "lemon/model_registry.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelManager;
using lemon::RegistryFile;
using lemon::RemoteRegistrySource;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "model_registry_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void test_source_parsing_and_cache_names() {
    check("empty source preserves Hugging Face default",
          lemon::parse_remote_registry_source("") == RemoteRegistrySource::HuggingFace);
    check("HF alias parses",
          lemon::parse_remote_registry_source("HF") == RemoteRegistrySource::HuggingFace);
    check("ModelScope alias parses",
          lemon::parse_remote_registry_source("ms") == RemoteRegistrySource::ModelScope);
    check("Hugging Face cache layout stays backward compatible",
          lemon::registry_repo_cache_dir_name("org/repo", RemoteRegistrySource::HuggingFace) ==
              "models--org--repo");
    check("ModelScope cache namespace cannot collide with Hugging Face",
          lemon::registry_repo_cache_dir_name("org/repo", RemoteRegistrySource::ModelScope) ==
              "modelscope--models--org--repo");

    bool rejected = false;
    try {
        (void)lemon::parse_remote_registry_source("unknown");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check("unknown registry is rejected", rejected);
}

static void test_search_result_normalization() {
    const auto hf = lemon::normalize_registry_search_result(
        RemoteRegistrySource::HuggingFace,
        json{{"id", "org/model-GGUF"},
             {"downloads", 42},
             {"likes", 7},
             {"tags", json::array({"gguf", "text-generation"})},
             {"pipeline_tag", "text-generation"}});
    check("HF search result keeps canonical repo id", hf.repo_id == "org/model-GGUF");
    check("HF GGUF metadata is normalized", hf.has_gguf && hf.downloads == 42 && hf.likes == 7);
    check("registry result identifies repository type", hf.repository_type == "model");

    const auto ms = lemon::normalize_registry_search_result(
        RemoteRegistrySource::ModelScope,
        json{{"Id", "Qwen/Qwen-GGUF"},
             {"Description", "ModelScope fixture"},
             {"Downloads", 123},
             {"Tags", "GGUF"}});
    check("ModelScope PascalCase fields normalize", ms.repo_id == "Qwen/Qwen-GGUF" &&
          ms.description == "ModelScope fixture" && ms.downloads == 123);
    check("ModelScope source remains explicit", ms.source == RemoteRegistrySource::ModelScope);
    check("single-string ModelScope tags normalize", ms.tags.size() == 1 && ms.tags[0] == "GGUF");

    const auto ms_openapi = lemon::normalize_registry_search_result(
        RemoteRegistrySource::ModelScope,
        json{{"Path", "Qwen/Qwen3.6-27B-GGUF"},
             {"display_name", "Qwen3.6 27B GGUF"},
             {"tasks", json::array({"text-generation", "chat"})}});
    check("ModelScope Path can carry the complete repository id",
          ms_openapi.repo_id == "Qwen/Qwen3.6-27B-GGUF");
    check("ModelScope tasks array normalizes to the primary task",
          ms_openapi.task == "text-generation");

    const auto ms_library = lemon::normalize_registry_search_result(
        RemoteRegistrySource::ModelScope,
        json{{"id", "community/Qwen-GGUF"},
             {"tags", json::array({"library:gguf", "task:text-generation"})}});
    check("ModelScope library facet identifies GGUF", ms_library.has_gguf);
    check("ModelScope task tag is normalized", ms_library.task == "text-generation");
}

static void test_search_response_normalization() {
    const json hf_body = json::array({
        json{{"id", "org/text-GGUF"}, {"pipeline_tag", "text-generation"},
             {"tags", json::array({"gguf"})}},
        json{{"id", "org/image-GGUF"}, {"pipeline_tag", "text-to-image"},
             {"tags", json::array({"gguf"})}},
    });
    const auto hf = lemon::normalize_registry_search_response(
        RemoteRegistrySource::HuggingFace, hf_body, 12);
    check("HF response filters unsupported media pipelines", hf.results.size() == 1);
    check("HF response preserves GGUF metadata", hf.results[0].has_gguf);

    const json ms_body = {
        {"data", {
            {"models", json::array({
                json{{"id", "Qwen/Qwen-GGUF"}, {"task", "text-generation"}, {"downloads", 9}},
                json{{"id", "Qwen/Qwen-Image"}, {"task", "text-to-image"}},
            })},
            {"total_count", 2},
        }},
    };
    const auto ms = lemon::normalize_registry_search_response(
        RemoteRegistrySource::ModelScope, ms_body, 12);
    check("ModelScope data.models envelope normalizes", ms.results.size() == 1 &&
          ms.results[0].repo_id == "Qwen/Qwen-GGUF");
    check("ModelScope response preserves provider total", ms.total == 2);
    check("ModelScope normalized result preserves source", ms.results[0].source == RemoteRegistrySource::ModelScope);

    const json lowercase_ms_body = {
        {"success", true},
        {"data", {
            {"models", json::array({
                json{{"Path", "community/Qwen3.6-GGUF"},
                     {"tasks", json::array({"text-generation"})}},
            })},
            {"total_count", 1},
        }},
    };
    const auto lowercase_ms = lemon::normalize_registry_search_response(
        RemoteRegistrySource::ModelScope, lowercase_ms_body, 12);
    check("lowercase ModelScope success envelope normalizes",
          lowercase_ms.results.size() == 1 &&
          lowercase_ms.results[0].repo_id == "community/Qwen3.6-GGUF");
}

static void test_tree_snapshot_fingerprint() {
    std::vector<RegistryFile> files = {
        {"model.gguf", 100, "sha256", "abc", false},
        {"config.json", 20, "sha256", "def", false},
    };
    const std::string first = lemon::registry_tree_snapshot_id(
        RemoteRegistrySource::ModelScope, "master", files);

    std::reverse(files.begin(), files.end());
    const std::string reordered = lemon::registry_tree_snapshot_id(
        RemoteRegistrySource::ModelScope, "master", files);
    check("snapshot fingerprint is independent of API file ordering", first == reordered);

    files[0].size += 1;
    const std::string changed = lemon::registry_tree_snapshot_id(
        RemoteRegistrySource::ModelScope, "master", files);
    check("snapshot fingerprint changes when the remote tree changes", first != changed);
    check("snapshot id records provider and revision",
          first.rfind("modelscope-master-", 0) == 0);
}

static void test_registration_persists_remote_provenance() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    ModelManager manager;
    manager.register_user_model("user.FromModelScope", json{
        {"source", "modelscope"},
        {"checkpoint", "org/repo:Q4_K_M"},
        {"recipe", "llamacpp"},
    });
    auto remote = manager.get_model_info("user.FromModelScope");
    check("remote source is persisted separately from local origin",
          remote.source.empty() && remote.registry_source == "modelscope");

    manager.register_user_model("user.LocalMirror", json{
        {"source", "local_upload"},
        {"registry_source", "modelscope"},
        {"checkpoint", "org/repo:Q4_K_M"},
        {"recipe", "llamacpp"},
    });
    auto local = manager.get_model_info("user.LocalMirror");
    check("local origin and remote provenance can coexist",
          local.source == "local_upload" && local.registry_source == "modelscope");

    fs::remove_all(temp);
}

int main() {
    test_source_parsing_and_cache_names();
    test_search_result_normalization();
    test_search_response_normalization();
    test_tree_snapshot_fingerprint();
    test_registration_persists_remote_provenance();

    if (g_failures == 0) {
        std::printf("All model registry tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
