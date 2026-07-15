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
    test_tree_snapshot_fingerprint();
    test_registration_persists_remote_provenance();

    if (g_failures == 0) {
        std::printf("All model registry tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
