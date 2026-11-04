#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "show_all_models.h"

namespace fs = std::filesystem;
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
    dir /= "recipe_options_" +
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
    if (!fs::exists(path)) return json();
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return json::parse(buffer.str(), nullptr, false);
}

static std::string hf_slug(const std::string& repo) {
    std::string slug = "models--";
    for (char c : repo) {
        if (c == '/') slug += "--";
        else slug += c;
    }
    return slug;
}

static void make_hf_repo(const fs::path& models_dir, const std::string& repo,
                         const std::string& snapshot, const std::string& file) {
    write_file(models_dir / hf_slug(repo) / "refs" / "main", snapshot);
    write_file(models_dir / hf_slug(repo) / "snapshots" / snapshot / file, "GGUF0123456789");
}

// A built-in name to key options against, taken from the shipped registry so the
// migration case does not depend on any particular model staying in it. Recipe
// matters: RecipeOptions keeps only the keys its recipe declares, so folding a
// saved ctx_size needs an llamacpp entry.
static std::string first_builtin_with_recipe(const std::string& want) {
    std::ifstream in(path_from_utf8(get_resource_path("resources/server_models.json")));
    std::stringstream buffer;
    buffer << in.rdbuf();
    json parsed = json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return {};
    for (const auto& [name, entry] : parsed.items()) {
        if (entry.is_object() && entry.value("recipe", "") == want) return name;
    }
    return {};
}

struct Fixture {
    fs::path root;
    fs::path models_dir;
    fs::path options_file;
    std::string builtin_name;
};

static Fixture create_fixture(const std::string& seeded_options = "") {
    Fixture fx;
    fx.root = make_temp_dir();
    fx.models_dir = fx.root / "hf";
    fx.options_file = fx.root / "recipe_options.json";
    fs::create_directories(fx.models_dir);

    make_hf_repo(fx.models_dir, "zzopts/zzrepo", "snap-o", "main.gguf");
    write_file(fx.root / "user_models.json",
               json{{"zz-opts",
                     json{{"checkpoint", "zzopts/zzrepo:main.gguf"}, {"recipe", "llamacpp"}}}}
                   .dump(2));

    fx.builtin_name = first_builtin_with_recipe("llamacpp");
    if (!seeded_options.empty()) write_file(fx.options_file, seeded_options);
    return fx;
}

static void use_fixture_dirs(const Fixture& fx) {
    lemon::utils::set_cache_dir(path_to_utf8(fx.root));
    lemon::utils::set_config_dir(path_to_utf8(fx.root));
    lemon::utils::set_models_dir(path_to_utf8(fx.models_dir));
}

static void test_replace_and_read_back(const Fixture& fx) {
    ModelManager manager;
    json saved = manager.replace_options(
        "user.zz-opts", json{{"ctx_size", 4096}, {"batch_size", 128}});
    check("replace returns the new entry",
          saved.value("ctx_size", 0) == 4096 && saved.value("batch_size", 0) == 128);

    json on_disk = read_json(fx.options_file);
    check("replace persists under the canonical id",
          on_disk.contains("user.zz-opts") &&
              on_disk["user.zz-opts"].value("ctx_size", 0) == 4096);

    json read_back = manager.options("user.zz-opts")["saved"];
    check("read back returns only the user's own entry",
          read_back.size() == 2 && read_back.value("ctx_size", 0) == 4096);

    check("a bare name reaches the same entry",
          manager.options("zz-opts")["saved"].value("ctx_size", 0) == 4096);
    check("an unknown model has no saved entry",
          manager.options("user.zz-nothing")["saved"].empty());
}

static void test_replace_overwrites_and_empty_erases(const Fixture& fx) {
    ModelManager manager;
    manager.replace_options("user.zz-opts", json{{"ctx_size", 4096}, {"keep", 1}});

    json replaced =
        manager.replace_options("user.zz-opts", json{{"ctx_size", 2048}});
    check("replace discards keys the previous entry had",
          replaced.size() == 1 && replaced.value("ctx_size", 0) == 2048);

    json emptied = manager.replace_options("user.zz-opts", json::object());
    check("replacing with an empty object returns an empty entry", emptied.empty());
    check("replacing with an empty object removes the row from disk",
          !read_json(fx.options_file).contains("user.zz-opts"));
}

static void test_merge_semantics(const Fixture& fx) {
    ModelManager manager;
    manager.replace_options("user.zz-opts",
                                    json{{"ctx_size", 4096}, {"batch_size", 64}});

    json merged = manager.set_options("user.zz-opts", json{{"ctx_size", 8192}});
    check("merge keeps untouched keys",
          merged.value("ctx_size", 0) == 8192 && merged.value("batch_size", 0) == 64);

    json nulled = manager.set_options("user.zz-opts", json{{"batch_size", nullptr}});
    check("a null change removes just that option",
          nulled.size() == 1 && nulled.value("ctx_size", 0) == 8192 && !nulled.contains("batch_size"));

    json on_disk = read_json(fx.options_file)["user.zz-opts"];
    check("the null removal is persisted", on_disk.size() == 1 && !on_disk.contains("batch_size"));

    json all_null = manager.set_options("user.zz-opts", json{{"ctx_size", nullptr}});
    check("nulling every option yields an empty entry", all_null.empty());
    check("nulling every option removes the row from disk",
          !read_json(fx.options_file).contains("user.zz-opts"));

    json fresh = manager.set_options("user.zz-other", json{{"ctx_size", 512}});
    check("merging onto no entry creates it", fresh.value("ctx_size", 0) == 512);
}

static void test_options_reach_model_info(const Fixture& fx) {
    ModelManager manager;
    manager.replace_options("user.zz-opts", json{{"ctx_size", 3072}});
    check("a saved option is visible in the model info without a rebuild",
          manager.get_model_info("user.zz-opts").recipe_options.get_option("ctx_size") == 3072);

    manager.set_options("user.zz-opts", json{{"ctx_size", 1024}});
    check("an updated option is visible in the model info",
          manager.get_model_info("user.zz-opts").recipe_options.get_option("ctx_size") == 1024);
}

// A model the cache never heard of has no info to patch, so the write must still
// persist and the cache must be rebuilt to pick the entry up.
static void test_options_for_unknown_model(const Fixture& fx) {
    ModelManager manager;
    json saved = manager.replace_options("user.zz-absent", json{{"ctx_size", 999}});
    check("options for an unknown model are still written", saved.value("ctx_size", 0) == 999);
    check("options for an unknown model persist",
          read_json(fx.options_file)["user.zz-absent"].value("ctx_size", 0) == 999);
}

static void test_persistence_across_instances(const Fixture& fx) {
    {
        ModelManager manager;
        manager.replace_options("user.zz-opts", json{{"ctx_size", 7777}});
    }
    ModelManager restarted;
    check("a new instance reads the saved entry",
          restarted.options("user.zz-opts")["saved"].value("ctx_size", 0) == 7777);
    check("a new instance folds the saved entry into the model info",
          restarted.get_model_info("user.zz-opts").recipe_options.get_option("ctx_size") == 7777);
}

static void test_key_migration() {
    const std::string legacy_key = first_builtin_with_recipe("llamacpp");
    const std::string written = R"({
        ")" + legacy_key + R"(": {"ctx_size": 2048},
        "user.zz-opts": {"ctx_size": 3000},
        "Stale-Built-In": {"ctx_size": 4000}
    })";
    Fixture seeded = create_fixture(written);
    use_fixture_dirs(seeded);

    ModelManager manager;
    json on_disk = read_json(seeded.options_file);
    check("a bare built-in key is rewritten with the builtin. prefix",
          on_disk.contains("builtin." + legacy_key) && !on_disk.contains(legacy_key));
    check("an already canonical key is left alone",
          on_disk.contains("user.zz-opts") &&
              on_disk["user.zz-opts"].value("ctx_size", 0) == 3000);
    check("an unknown bare key is preserved rather than dropped",
          on_disk.contains("Stale-Built-In") &&
              on_disk["Stale-Built-In"].value("ctx_size", 0) == 4000);
    check("the migrated entry is readable",
          manager.options(legacy_key)["saved"].value("ctx_size", 0) == 2048);
    check("the migrated entry folds into the built-in's model info",
          manager.get_model_info(legacy_key).recipe_options.get_option("ctx_size") == 2048);

    fs::remove_all(seeded.root);
}

static void test_corrupt_file_is_not_fatal() {
    Fixture seeded = create_fixture(R"([1, 2, 3])");
    use_fixture_dirs(seeded);

    ModelManager manager;
    check("a non-object options file reads as empty",
          manager.options("user.zz-opts")["saved"].empty());
    json saved = manager.replace_options("user.zz-opts", json{{"ctx_size", 123}});
    check("writing over a corrupt file succeeds", saved.value("ctx_size", 0) == 123);
    check("the corrupt file is replaced by an object",
          read_json(seeded.options_file).is_object() &&
              read_json(seeded.options_file)["user.zz-opts"].value("ctx_size", 0) == 123);

    fs::remove_all(seeded.root);
}

int main() {
    ShowAllModels show_all_models;
    Fixture fx = create_fixture();
    use_fixture_dirs(fx);

    {
        ModelManager manager;
        test_replace_and_read_back(fx);
        test_replace_overwrites_and_empty_erases(fx);
        test_merge_semantics(fx);
        test_options_reach_model_info(fx);
        test_options_for_unknown_model(fx);
    }

    fs::remove(fx.options_file);
    {
        test_persistence_across_instances(fx);
    }

    test_key_migration();
    test_corrupt_file_is_not_fatal();

    fs::remove_all(fx.root);

    if (g_failures == 0) {
        std::printf("All recipe-options-persistence tests passed.\n");
    } else {
        std::printf("%d recipe-options-persistence test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
