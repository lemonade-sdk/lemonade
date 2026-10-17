#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "show_all_models.h"

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::json;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// Each case performs one supported mutation, then asks a lock-free reader: what
// breaks if a mutating scope forgets to publish.
static std::string listing_name_of(const std::map<std::string, ModelInfo>& models,
                                   const std::string& repo) {
    for (const auto& [name, info] : models) {
        for (const auto& [type, checkpoint] : info.checkpoints) {
            if (checkpoint.find(repo) != std::string::npos) {
                return name;
            }
        }
    }
    return std::string();
}

static bool listing_is_self_consistent(const std::map<std::string, ModelInfo>& models) {
    for (const auto& [name, info] : models) {
        if (name != info.model_name) {
            return false;
        }
    }
    return true;
}

static void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// llamacpp checks the GGUF magic before it calls a file downloaded.
static fs::path make_hf_repo(const fs::path& models_dir, const std::string& repo,
                             const std::string& snapshot, const std::string& file) {
    std::string slug;
    for (char c : repo) {
        if (c == '/') slug += "--"; else slug += c;
    }
    fs::path repo_dir = models_dir / ("models--" + slug);
    write_file(repo_dir / "refs" / "main", snapshot);
    write_file(repo_dir / "snapshots" / snapshot / file, "GGUF0123456789");
    return repo_dir;
}

static void register_model(ModelManager& manager,
                           const std::string& name,
                           const std::string& recipe,
                           const std::string& checkpoint) {
    manager.register_user_model(name, json{{"model_name", name},
                                           {"recipe", recipe},
                                           {"checkpoint", checkpoint}});
}

static void test_registration_publishes(ModelManager& manager) {
    register_model(manager, "user.Published", "llamacpp", "zzorg/zzpublished:main.gguf");

    check("registering publishes the model to the existence check",
          manager.model_exists("user.Published"));
    check("registering publishes the model to the info lookup",
          manager.get_model_info("user.Published").recipe == "llamacpp");
    const std::string listed = listing_name_of(manager.get_supported_models(), "zzorg/zzpublished");
    check("registering publishes the model to the listing", !listed.empty());
    check("registering publishes a name that resolves back to the model",
          !listed.empty() && manager.get_model_info(listed).checkpoints.size() ==
                                 manager.get_model_info("user.Published").checkpoints.size());
    check("published listing keys match the names they carry",
          listing_is_self_consistent(manager.get_supported_models()));
}

static void test_deletion_publishes(ModelManager& manager) {
    manager.delete_model("user.Published");

    check("deleting publishes its absence from the existence check",
          !manager.model_exists("user.Published"));
    check("deleting publishes its absence from the listing",
          listing_name_of(manager.get_supported_models(), "zzorg/zzpublished").empty());
    bool threw = false;
    try {
        manager.get_model_info("user.Published");
    } catch (const std::exception&) {
        threw = true;
    }
    check("deleting publishes its absence from the info lookup", threw);
}

// Writing again through the same paths catches a store that publishes only once.
static void test_republish_after_replacement(ModelManager& manager) {
    register_model(manager, "user.Replaced", "llamacpp", "zzorg/zzfirst:main.gguf");
    check("the first definition is visible",
          manager.get_model_info("user.Replaced").checkpoints.count("main") == 1);

    manager.delete_model("user.Replaced");
    register_model(manager, "user.Replaced", "sd-cpp", "zzorg/zzsecond:vae.safetensors");

    const ModelInfo after = manager.get_model_info("user.Replaced");
    check("the replacement definition is visible", after.recipe == "sd-cpp");
    check("the previous definition is gone",
          listing_name_of(manager.get_supported_models(), "zzorg/zzfirst").empty());
    check("the listing stays self-consistent across replacement",
          listing_is_self_consistent(manager.get_supported_models()));
}

// Options are written into the cached entry in place with the cache still valid,
// so nothing rebuilds and publication is the only way the change can be seen.
static void test_in_place_update_publishes(ModelManager& manager) {
    register_model(manager, "user.Options", "llamacpp", "zzorg/zzoptions:main.gguf");

    // Chosen at run time so the case does not break when the option set changes.
    std::string key;
    for (const auto& candidate : lemon::RecipeOptions::keys_for_recipe("llamacpp")) {
        if (lemon::RecipeOptions("llamacpp", json{{candidate, json(4096)}}).get_option(candidate) ==
            json(4096)) {
            key = candidate;
            break;
        }
    }
    if (key.empty()) {
        check("llamacpp offers an option this test can set", false);
        return;
    }

    ModelInfo info = manager.get_model_info("user.Options");
    info.recipe_options.set_option(key, json(4096));
    manager.save_model_options(info);

    check("an in-place update publishes to readers",
          manager.get_model_info("user.Options").recipe_options.get_option(key) == json(4096));
}

// Deciding a model is no longer complete rewrites the cached entry, and nothing
// rebuilds on its own, so publication is the only way the repair reaches readers.
static void test_repair_publishes(ModelManager& manager, const fs::path& models_dir) {
    const fs::path repo = make_hf_repo(models_dir, "zzorg/zzvanishing", "snap-x", "main.gguf");
    register_model(manager, "user.Vanishing", "llamacpp", "zzorg/zzvanishing:main.gguf");
    check("fixture starts out downloaded", manager.is_model_downloaded("user.Vanishing"));

    fs::remove(repo / "snapshots" / "snap-x" / "main.gguf");

    check("the missing file is reported", !manager.is_model_downloaded("user.Vanishing"));
    check("the repair publishes to readers",
          !manager.get_model_info("user.Vanishing").downloaded);
    check("the repaired model leaves the downloaded listing",
          listing_name_of(manager.get_downloaded_models(), "zzorg/zzvanishing").empty());
}

// Readers run against the published snapshot while a writer keeps replacing it.
// Nothing is asserted mid-race: which generation a reader sees is timing.
static void test_republish_while_reading(ModelManager& manager) {
    manager.get_supported_models();

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const std::size_t clouds = manager.count_cloud_models("zz-no-provider");
                const std::set<std::string> hidden =
                    manager.recipes_all_models_filtered_snapshot();
                (void)clouds;
                (void)hidden;
                reads.fetch_add(1);
            }
        });
    }

    for (int i = 0; i < 500; ++i) {
        manager.invalidate_models_cache();
    }

    stop.store(true);
    for (auto& reader : readers) {
        reader.join();
    }

    check("readers were served throughout republication", reads.load() > 0);
    check("listing stays self-consistent after concurrent republication",
          listing_is_self_consistent(manager.get_supported_models()));
}

int main() {
    ShowAllModels show_all_models;
    fs::path temp = fs::temp_directory_path();
    temp /= "store_publication_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(temp);
    lemon::utils::set_cache_dir(path_to_utf8(temp));
    lemon::utils::set_config_dir(path_to_utf8(temp));
    fs::path models_dir = temp / "hf";
    fs::create_directories(models_dir);
    lemon::utils::set_models_dir(path_to_utf8(models_dir));

    ModelManager manager;
    test_registration_publishes(manager);
    test_deletion_publishes(manager);
    test_republish_after_replacement(manager);
    test_in_place_update_publishes(manager);
    test_repair_publishes(manager, models_dir);
    test_republish_while_reading(manager);

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All store publication tests passed.\n");
    } else {
        std::printf("%d store publication test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
