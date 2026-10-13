#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "model_deletion_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path& path, const std::string& contents = "data") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

static json read_json(const fs::path& path) {
    if (!fs::exists(path)) return json::object();
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    json parsed = json::parse(buffer.str(), nullptr, false);
    return parsed.is_discarded() ? json::object() : parsed;
}

// An HF snapshot checkout: <models>/models--<org>--<repo>/{refs/main,snapshots/<id>/<files>}
static fs::path make_hf_repo(const fs::path& models_dir,
                             const std::string& repo,
                             const std::string& snapshot,
                             const std::vector<std::string>& files) {
    std::string slug = "models--";
    for (char c : repo) {
        if (c == '/') slug += "--";
        else slug += c;
    }
    fs::path repo_dir = models_dir / slug;
    write_file(repo_dir / "refs" / "main", snapshot);
    for (const auto& file : files) {
        write_file(repo_dir / "snapshots" / snapshot / file, "GGUF");
    }
    return repo_dir;
}

struct Fixture {
    fs::path root;
    fs::path models_dir;
    fs::path user_models_file;

    fs::path hf_single;
    fs::path hf_shared;
    fs::path hf_main_repo;
    fs::path hf_aux_repo;
    fs::path imported_file;
    fs::path extra_dir;
    fs::path extra_file;
};

static Fixture create_fixture() {
    Fixture fx;
    fx.root = make_temp_dir();
    fx.models_dir = fx.root / "hf";
    fx.user_models_file = fx.root / "user_models.json";
    fs::create_directories(fx.models_dir);

    fx.hf_single = make_hf_repo(fx.models_dir, "zz/single", "snap-a", {"main.gguf"});
    fx.hf_shared =
        make_hf_repo(fx.models_dir, "zz/shared", "snap-s", {"a.gguf", "b.gguf"});
    fx.hf_main_repo = make_hf_repo(fx.models_dir, "zz/mainrepo", "snap-m", {"main.gguf"});
    fx.hf_aux_repo = make_hf_repo(fx.models_dir, "zzaux/auxrepo", "snap-x", {"proj.gguf"});

    fx.imported_file = fx.root / "imported" / "zz-model.gguf";
    write_file(fx.imported_file, "GGUF");
    fx.extra_dir = fx.root / "extra";
    fx.extra_file = fx.extra_dir / "zz-extra.gguf";
    write_file(fx.extra_file, "GGUF");

    json user_models = {
        {"zz-hf-single",
         json{{"checkpoint", "zz/single:main.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-shared-a",
         json{{"checkpoint", "zz/shared:a.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-shared-b",
         json{{"checkpoint", "zz/shared:b.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-multi-repo",
         json{{"checkpoints",
               json{{"main", "zz/mainrepo:main.gguf"},
                    {"mmproj", "zzaux/auxrepo:proj.gguf"}}},
              {"recipe", "llamacpp"}}},
        {"zz-never-downloaded",
         json{{"checkpoint", "zz/ghostrepo:main.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-imported",
         json{{"checkpoints", json{{"main", path_to_utf8(fx.imported_file)}}},
              {"recipe", "llamacpp"},
              {"source", "local_path"}}},
    };
    write_file(fx.user_models_file, user_models.dump(2));
    return fx;
}

// Returns true when the delete completed without throwing.
static bool delete_model(ModelManager& manager,
                         const std::string& name,
                         std::string& error) {
    try {
        manager.delete_model(name);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

static bool still_registered(const Fixture& fx, const std::string& key) {
    return read_json(fx.user_models_file).contains(key);
}

static bool gone_from_cache(ModelManager& manager, const std::string& name) {
    try {
        manager.get_model_info(name);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

static void test_registry_backed_delete(ModelManager& manager, const Fixture& fx) {
    check("single-repo model starts downloaded",
          manager.get_model_info("user.zz-hf-single").downloaded);

    std::string error;
    check("single-repo delete completes", delete_model(manager, "user.zz-hf-single", error));
    check("single-repo cache directory removed", !fs::exists(fx.hf_single));
    check("single-repo entry removed from user_models.json",
          !still_registered(fx, "zz-hf-single"));
    check("single-repo no longer resolves", gone_from_cache(manager, "user.zz-hf-single"));
}

static void test_shared_repo_keeps_other_variants(ModelManager& manager, const Fixture& fx) {
    check("shared repo: both models start downloaded",
          manager.get_model_info("user.zz-shared-a").downloaded &&
              manager.get_model_info("user.zz-shared-b").downloaded);

    std::string error;
    check("shared repo delete completes", delete_model(manager, "user.zz-shared-a", error));

    const fs::path snapshot = fx.hf_shared / "snapshots" / "snap-s";
    check("shared repo: deleted variant file removed", !fs::exists(snapshot / "a.gguf"));
    check("shared repo: other model's variant file kept", fs::exists(snapshot / "b.gguf"));
    check("shared repo: repository directory kept for the other model",
          fs::exists(fx.hf_shared));
    check("shared repo: only the named entry is unregistered",
          !still_registered(fx, "zz-shared-a") && still_registered(fx, "zz-shared-b"));
    check("shared repo: the other model still resolves downloaded",
          manager.get_model_info("user.zz-shared-b").downloaded);
}

static void test_multi_repo_delete_removes_every_owned_repo(
    ModelManager& manager, const Fixture& fx) {
    check("multi-repo model starts downloaded",
          manager.get_model_info("user.zz-multi-repo").downloaded);

    std::string error;
    check("multi-repo delete completes", delete_model(manager, "user.zz-multi-repo", error));
    check("multi-repo: main repository directory removed", !fs::exists(fx.hf_main_repo));
    check("multi-repo: auxiliary repository directory removed", !fs::exists(fx.hf_aux_repo));
    check("multi-repo entry removed from user_models.json",
          !still_registered(fx, "zz-multi-repo"));
}

static void test_registry_only_delete(ModelManager& manager, const Fixture& fx) {
    check("undownloaded model starts not downloaded",
          !manager.get_model_info("user.zz-never-downloaded").downloaded);

    std::string error;
    check("undownloaded delete completes",
          delete_model(manager, "user.zz-never-downloaded", error));
    check("undownloaded entry removed from user_models.json",
          !still_registered(fx, "zz-never-downloaded"));
}

// Spec: a model whose bytes live outside any cache directory unregisters cleanly and
// its bytes stay (model-management.md, Deletion rules 2-3). Today delete_model() walks
// up for a models-- ancestor and throws instead, so the entry survives: this is the
// defect step 9 removes.
static void test_imported_model_delete(ModelManager& manager, const Fixture& fx) {
    ModelInfo imported = manager.get_model_info("user.zz-imported");
    check("imported model resolves to the user's own path",
          path_from_utf8(imported.resolved_path()) == fx.imported_file);
    check("imported model starts downloaded", imported.downloaded);

    std::string error;
    const bool completed = delete_model(manager, "user.zz-imported", error);
    check("imported delete completes without throwing", completed);
    if (!completed) {
        std::printf("       reported: %s\n", error.c_str());
    }
    check("imported entry removed from user_models.json",
          !still_registered(fx, "zz-imported"));
    check("imported delete leaves the user's bytes on disk", fs::exists(fx.imported_file));
    check("imported model no longer resolves", gone_from_cache(manager, "user.zz-imported"));
}

// Extra-directory bytes are user-managed, so a delete request refuses rather than
// removing them and points the caller at the file. Intended behaviour, not a gap.
static void test_extra_model_delete(ModelManager& manager, const Fixture& fx) {
    check("extra model is discovered", !gone_from_cache(manager, "extra.zz-extra"));

    std::string error;
    const bool completed = delete_model(manager, "extra.zz-extra", error);
    check("extra model bytes are never deleted by a delete request",
          fs::exists(fx.extra_file));
    check("extra model delete is refused, naming the directory",
          !completed && error.find("extra") != std::string::npos);
    check("refused delete leaves the model resolvable", !gone_from_cache(manager, "extra.zz-extra"));
}

int main() {
    Fixture fx = create_fixture();

    lemon::utils::set_cache_dir(path_to_utf8(fx.root));
    lemon::utils::set_config_dir(path_to_utf8(fx.root));
    lemon::utils::set_models_dir(path_to_utf8(fx.models_dir));

    {
        ModelManager manager(path_to_utf8(fx.extra_dir));
        test_registry_backed_delete(manager, fx);
        test_shared_repo_keeps_other_variants(manager, fx);
        test_multi_repo_delete_removes_every_owned_repo(manager, fx);
        test_registry_only_delete(manager, fx);
        test_imported_model_delete(manager, fx);
        test_extra_model_delete(manager, fx);
    }

    fs::remove_all(fx.root);

    if (g_failures == 0) {
        std::printf("All model-deletion tests passed.\n");
    } else {
        std::printf("%d model-deletion test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
