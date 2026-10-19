#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"
#include "show_all_models.h"
#include "support/server_wired_manager.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelFileInfo;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::json;
using lemon::utils::get_resource_path;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "store_snapshot_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path& path, const std::string& contents = "data") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// llamacpp validates the GGUF magic on a resolved file, so a fixture that must read
// as downloaded needs the header; a non-GGUF body reads as present but incomplete.
static const char kGguf[] = "GGUF0123456789";
static constexpr std::uint64_t kGgufSize = 14;
static const char kNotGguf[] = "<html>404</html>";

static std::string hf_slug(const std::string& repo) {
    std::string slug = "models--";
    for (char c : repo) {
        if (c == '/') slug += "--";
        else slug += c;
    }
    return slug;
}

static fs::path make_hf_repo(const fs::path& models_dir,
                             const std::string& repo,
                             const std::string& snapshot,
                             const std::vector<std::string>& files) {
    fs::path repo_dir = models_dir / hf_slug(repo);
    write_file(repo_dir / "refs" / "main", snapshot);
    for (const auto& file : files) {
        write_file(repo_dir / "snapshots" / snapshot / file, kGguf);
    }
    return repo_dir;
}

// A built-in name to shadow, read from the shipped registry so the precedence case
// does not depend on any particular model staying in server_models.json.
static std::string first_builtin_name() {
    std::ifstream in(path_from_utf8(get_resource_path("resources/server_models.json")));
    std::stringstream buffer;
    buffer << in.rdbuf();
    json parsed = json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.empty()) return {};
    return parsed.begin().key();
}

struct Fixture {
    fs::path root;
    fs::path models_dir;
    std::string shadowed_builtin;

    fs::path variant_repo;
    fs::path plain_repo;
    fs::path main_repo;
    fs::path aux_repo;
    fs::path local_file;
    fs::path corrupt_file;
    fs::path stray_aux_copy;
};

static Fixture create_fixture() {
    Fixture fx;
    fx.root = make_temp_dir();
    fx.models_dir = fx.root / "hf";
    fs::create_directories(fx.models_dir);

    fx.variant_repo = make_hf_repo(fx.models_dir, "zzorg/zzvariant", "snap-v", {"zz-q4.gguf"});
    fx.plain_repo = make_hf_repo(fx.models_dir, "zzorg/zzplain", "snap-p", {"config.json"});
    fx.main_repo = make_hf_repo(fx.models_dir, "zzorg/zzmain", "snap-m", {"main.gguf"});
    fx.aux_repo = make_hf_repo(fx.models_dir, "zzorg/zzaux", "snap-a", {"proj.gguf"});

    fx.local_file = fx.root / "imported" / "zz-local.gguf";
    write_file(fx.local_file, kGguf);
    fx.corrupt_file = fx.root / "imported" / "zz-corrupt.gguf";
    write_file(fx.corrupt_file, kNotGguf);

    // Legacy layout: an auxiliary checkpoint's file left inside the main repo cache.
    fx.stray_aux_copy = fx.main_repo / "snapshots" / "snap-m" / "proj.gguf";
    write_file(fx.stray_aux_copy, "abcdefghij");

    fx.shadowed_builtin = first_builtin_name();

    json user_models = {
        {"zz-variant",
         json{{"checkpoint", "zzorg/zzvariant:zz-q4.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-plain", json{{"checkpoint", "zzorg/zzplain"}, {"recipe", "sd-cpp"}}},
        {"zz-multi",
         json{{"checkpoints",
               json{{"main", "zzorg/zzmain:main.gguf"},
                    {"mmproj", "zzorg/zzaux:proj.gguf"}}},
              {"recipe", "llamacpp"}}},
        {"zz-same-repo",
         json{{"checkpoints",
               json{{"main", "zzorg/zzplain"}, {"vae", "zzorg/zzplain"}}},
              {"recipe", "sd-cpp"}}},
        {"zz-local",
         json{{"checkpoints", json{{"main", path_to_utf8(fx.local_file)}}},
              {"recipe", "llamacpp"},
              {"source", "local_path"}}},
        {"zz-corrupt",
         json{{"checkpoints", json{{"main", path_to_utf8(fx.corrupt_file)}}},
              {"recipe", "llamacpp"},
              {"source", "local_path"}}},
        {"zz-undownloaded",
         json{{"checkpoint", "zzorg/zzghost:main.gguf"}, {"recipe", "llamacpp"}}},
    };
    if (!fx.shadowed_builtin.empty()) {
        user_models[fx.shadowed_builtin] =
            json{{"checkpoint", "zzorg/zzshadow:main.gguf"}, {"recipe", "llamacpp"}};
    }
    write_file(fx.root / "user_models.json", user_models.dump(2));
    return fx;
}

// Resolved paths embed a fresh temp directory on every run, so the golden records a
// registry path relative to the models dir and a signature for an imported one.
static std::string rel(const fs::path& models_dir, const std::string& path) {
    if (path.empty()) return "<empty>";
    if (path.find("imported") != std::string::npos) return "<import>";
    fs::path as_path = path_from_utf8(path);
    if (!as_path.is_absolute()) return path;
    return path_to_utf8(as_path.lexically_relative(models_dir));
}

static std::string snapshot_signature(const std::map<std::string, ModelInfo>& models,
                                      const Fixture& fx) {
    std::ostringstream out;
    for (const auto& [id, info] : models) {
        if (id.compare(0, 3, "zz-") != 0) continue;
        out << id << " recipe=" << info.recipe << " source=" << info.source
            << " downloaded=" << (info.downloaded ? "yes" : "no") << " ckpt={";
        for (const auto& [type, checkpoint] : info.checkpoints) {
            out << type << ":" << rel(fx.models_dir, checkpoint) << ",";
        }
        out << "} paths={";
        for (const auto& [type, resolved] : info.resolved_paths) {
            out << type << ":" << rel(fx.models_dir, resolved) << ",";
        }
        out << "}\n";
    }
    return out.str();
}

static const char* kExpectedSnapshot =
    R"GOLDEN(zz-corrupt recipe=llamacpp source=local_path downloaded=no ckpt={main:<import>,} paths={main:<import>,}
zz-local recipe=llamacpp source=local_path downloaded=yes ckpt={main:<import>,} paths={main:<import>,}
zz-multi recipe=llamacpp source= downloaded=yes ckpt={main:zzorg/zzmain:main.gguf,mmproj:zzorg/zzaux:proj.gguf,} paths={main:models--zzorg--zzmain/snapshots/snap-m/main.gguf,mmproj:models--zzorg--zzaux/snapshots/snap-a/proj.gguf,}
zz-plain recipe=sd-cpp source= downloaded=yes ckpt={main:zzorg/zzplain,} paths={main:models--zzorg--zzplain/snapshots/snap-p,}
zz-same-repo recipe=sd-cpp source= downloaded=yes ckpt={main:zzorg/zzplain,vae:zzorg/zzplain,} paths={main:models--zzorg--zzplain/snapshots/snap-p,vae:models--zzorg--zzplain/snapshots/snap-p,}
zz-undownloaded recipe=llamacpp source= downloaded=no ckpt={main:zzorg/zzghost:main.gguf,} paths={main:models--zzorg--zzghost,}
zz-variant recipe=llamacpp source= downloaded=yes ckpt={main:zzorg/zzvariant:zz-q4.gguf,} paths={main:models--zzorg--zzvariant/snapshots/snap-v/zz-q4.gguf,}
)GOLDEN";

static void test_published_snapshot(ModelManager& manager, const Fixture& fx) {
    const std::string actual = snapshot_signature(manager.get_supported_models(), fx);
    const bool matches = actual == kExpectedSnapshot;
    check("published snapshot matches golden", matches);
    if (!matches) {
        std::printf("--- actual ---\n%s--- golden ---\n%s----------------\n",
                    actual.c_str(), kExpectedSnapshot);
    }
}

// get_supported_models() is keyed by the bare listing name and carries that name in
// ModelInfo::model_name, while a lookup by canonical id reports the canonical name back.
// Identity across the two views is therefore content, not the name string.
static void test_canonical_identity(ModelManager& manager, const Fixture& fx) {
    auto models = manager.get_supported_models();
    bool identity_ok = true;
    int checked = 0;
    for (const auto& [id, info] : models) {
        if (id.compare(0, 3, "zz-") != 0) continue;
        ++checked;
        const std::string canonical = manager.resolve_model_name(id);
        const ModelInfo by_canonical = manager.get_model_info(canonical);
        if (canonical != "user." + id || by_canonical.checkpoints != info.checkpoints ||
            by_canonical.recipe != info.recipe) {
            identity_ok = false;
        }
    }
    check("each listed fixture name resolves to its user. canonical id",
          checked == 7 && identity_ok);
}

static void test_precedence_and_public_naming(ModelManager& manager, const Fixture& fx) {
    if (fx.shadowed_builtin.empty()) {
        check("shadowed built-in name discovered", false);
        return;
    }
    const std::string user_id = "user." + fx.shadowed_builtin;
    check("user entry shadows the built-in of the same bare name",
          manager.resolve_model_name(fx.shadowed_builtin) == user_id);
    check("shadowing model is listed under the bare name",
          manager.get_public_model_name(user_id) == fx.shadowed_builtin);
    check("shadowed built-in keeps a qualified listing name",
          manager.get_public_model_name(fx.shadowed_builtin) != fx.shadowed_builtin);
    check("shadowing model wins the info lookup",
          manager.get_model_info(fx.shadowed_builtin).checkpoint() ==
              "zzorg/zzshadow:main.gguf");
}

static void test_public_names_resolve(ModelManager& manager) {
    auto models = manager.get_supported_models();
    bool every_entry_has_a_recipe = true;
    for (const auto& [id, info] : models) {
        (void)id;
        if (info.recipe.empty()) every_entry_has_a_recipe = false;
    }
    check("no published entry has an empty recipe", every_entry_has_a_recipe);

    size_t rows_from_shipped_registry = 0;
    for (const auto& [id, info] : models) {
        (void)info;
        if (id.compare(0, 3, "zz-") != 0) ++rows_from_shipped_registry;
    }
    check("the shipped registry is published alongside the fixtures",
          rows_from_shipped_registry > 0);
}

static void test_list_model_files(ModelManager& manager, const Fixture& fx) {
    std::vector<ModelFileInfo> files = manager.list_model_files("user.zz-multi");
    check("one row per resolved checkpoint", files.size() == 2);

    bool main_ok = false;
    bool mmproj_ok = false;
    for (const auto& file : files) {
        if (file.role == "main") {
            main_ok = file.exists && file.name == "main.gguf" &&
                      file.size_bytes == kGgufSize &&
                      path_from_utf8(file.path) ==
                          fx.main_repo / "snapshots" / "snap-m" / "main.gguf";
        } else if (file.role == "mmproj") {
            mmproj_ok = file.exists && file.name == "proj.gguf" &&
                        path_from_utf8(file.path) ==
                            fx.aux_repo / "snapshots" / "snap-a" / "proj.gguf";
        }
    }
    check("main row describes the resolved main file", main_ok);
    check("mmproj row describes the resolved auxiliary file", mmproj_ok);

    std::vector<ModelFileInfo> local_files = manager.list_model_files("user.zz-local");
    check("local import reports one row", local_files.size() == 1);
    check("local import reports its real byte size",
          !local_files.empty() && local_files[0].size_bytes == kGgufSize &&
              local_files[0].exists);

    fs::remove(fx.local_file);
    manager.invalidate_models_cache();
    std::vector<ModelFileInfo> missing = manager.list_model_files("user.zz-local");
    check("a vanished file is reported as absent with no size",
          missing.size() == 1 && !missing[0].exists && missing[0].size_bytes == 0);
    write_file(fx.local_file, kGguf);

    // Presence and completeness are different questions: the file is on disk and is
    // inventoried, yet the model is not downloaded because it is not a GGUF file.
    std::vector<ModelFileInfo> corrupt = manager.list_model_files("user.zz-corrupt");
    check("a file without GGUF magic is still inventoried as present",
          corrupt.size() == 1 && corrupt[0].exists && corrupt[0].size_bytes > 0);
    check("a file without GGUF magic keeps the model not downloaded",
          !manager.get_model_info("user.zz-corrupt").downloaded);
}

static void test_cleanup_orphaned_cache(ModelManager& manager, const Fixture& fx) {
    json dry = manager.cleanup_orphaned_cache(true);
    const auto& found = dry["orphaned_files"];
    check("dry run reports the stray auxiliary file", found.size() == 1);
    if (found.size() == 1) {
        const json& entry = found[0];
        check("orphan is attributed to the checkpoint that owns it",
              entry["model"] == "user.zz-multi" && entry["type"] == "mmproj" &&
                  entry["belongs_to"] == "zzorg/zzaux");
        check("orphan size is reported", entry["size"] == 10);
        check("total_bytes aggregates orphans", dry["total_bytes"] == 10);
        check("dry run is marked as such", dry["dry_run"] == true);
    }
    check("dry run deletes nothing", fs::exists(fx.stray_aux_copy));
    check("dry run leaves the real checkpoint file alone",
          fs::exists(fx.main_repo / "snapshots" / "snap-m" / "main.gguf"));

    json run = manager.cleanup_orphaned_cache(false);
    check("cleanup reports the same orphan it deletes",
          run["orphaned_files"].size() == 1 && run["dry_run"] == false);
    check("cleanup removes the stray file", !fs::exists(fx.stray_aux_copy));
    check("cleanup keeps the checkpoint's own file in the main repo",
          fs::exists(fx.main_repo / "snapshots" / "snap-m" / "main.gguf"));
    check("cleanup keeps the auxiliary repository untouched", fs::exists(fx.aux_repo));

    check("second cleanup pass finds nothing",
          manager.cleanup_orphaned_cache(true)["orphaned_files"].empty());
}

int main() {
    ShowAllModels show_all_models;
    Fixture fx = create_fixture();

    lemon::utils::set_cache_dir(path_to_utf8(fx.root));
    lemon::utils::set_config_dir(path_to_utf8(fx.root));
    lemon::utils::set_models_dir(path_to_utf8(fx.models_dir));

    {
        ModelManager manager(lemon::test::server_wired_config());
        test_published_snapshot(manager, fx);
        test_canonical_identity(manager, fx);
        test_precedence_and_public_naming(manager, fx);
        test_public_names_resolve(manager);
        test_list_model_files(manager, fx);
        test_cleanup_orphaned_cache(manager, fx);
    }

    fs::remove_all(fx.root);

    if (g_failures == 0) {
        std::printf("All store-snapshot tests passed.\n");
    } else {
        std::printf("%d store-snapshot test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
