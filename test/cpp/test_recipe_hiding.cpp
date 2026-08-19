// Tests for the rule that reports a backend as unavailable (so clients hide it
// from the recipe/backends listing) when every one of its built-in models was
// dropped by the system-memory heuristic. Two layers are covered:
//   * the pure set-difference at the heart of the rule, and
//   * the stateful contract that only a full-registry filter pass may commit the
//     availability side table, so incremental single-model updates cannot
//     clobber it.

#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo model(const std::string& recipe, double size_gb) {
    ModelInfo info;
    info.recipe = recipe;
    info.size = size_gb;
    return info;
}

static void test_pure_set_difference() {
    using S = std::set<std::string>;

    check("all built-ins filtered -> recipe reported",
          ModelManager::recipes_missing_all_models({"ds4"}, {}) == S{"ds4"});

    check("partial filtering -> recipe kept",
          ModelManager::recipes_missing_all_models({"llamacpp"}, {"llamacpp"}) == S{});

    check("no filtered models -> nothing reported",
          ModelManager::recipes_missing_all_models({}, {"llamacpp", "kokoro"}) == S{});

    check("mixed recipes resolve independently",
          ModelManager::recipes_missing_all_models({"ds4", "vllm"}, {"vllm", "llamacpp"})
              == S{"ds4"});

    check("recipe not size-filtered is never reported",
          ModelManager::recipes_missing_all_models({"ds4"}, {"ds4", "llamacpp"}) == S{});
}

// A size so large no real machine can hold it, so the model is always
// memory-filtered regardless of where the test runs.
static constexpr double HUGE_GB = 1.0e6;

static void test_full_pass_commits_and_recomputes(ModelManager& manager) {
    // Full pass over a registry where whispercpp's only model is unfillable and
    // kokoro's fits: whispercpp is reported (if supported on this host), kokoro
    // is not.
    std::map<std::string, ModelInfo> full = {
        {"HugeWhisper", model("whispercpp", HUGE_GB)},
        {"TinyKokoro", model("kokoro", 0.01)},
    };
    manager.filter_models_by_backend(full, /*track_recipe_availability=*/true);
    std::set<std::string> after_huge = manager.recipes_all_models_filtered_snapshot();
    check("full pass never reports a recipe that kept a visible model",
          after_huge.count("kokoro") == 0);

    // A subsequent full pass where whispercpp now has a fitting model must
    // recompute from scratch, not accumulate: whispercpp is no longer reported.
    std::map<std::string, ModelInfo> full_fits = {
        {"TinyWhisper", model("whispercpp", 0.01)},
        {"TinyKokoro", model("kokoro", 0.01)},
    };
    manager.filter_models_by_backend(full_fits, /*track_recipe_availability=*/true);
    std::set<std::string> after_fits = manager.recipes_all_models_filtered_snapshot();
    check("full pass recomputes: a now-fitting recipe is dropped",
          after_fits.count("whispercpp") == 0);
}

static void test_incremental_pass_does_not_clobber(ModelManager& manager) {
    // Seed the side table with a full pass, then run an incremental single-model
    // pass (as add_model_to_cache does). The incremental pass must leave the
    // side table untouched.
    std::map<std::string, ModelInfo> full = {
        {"HugeWhisper", model("whispercpp", HUGE_GB)},
    };
    manager.filter_models_by_backend(full, /*track_recipe_availability=*/true);
    std::set<std::string> seeded = manager.recipes_all_models_filtered_snapshot();

    std::map<std::string, ModelInfo> one = {{"TinyKokoro", model("kokoro", 0.01)}};
    manager.filter_models_by_backend(one, /*track_recipe_availability=*/false);
    std::set<std::string> after_incremental = manager.recipes_all_models_filtered_snapshot();

    check("incremental pass leaves the availability side table unchanged",
          after_incremental == seeded);
}

int main() {
    fs::path temp = fs::temp_directory_path() / "lemonade-recipe-hiding-test";
    fs::create_directories(temp);
    lemon::utils::set_cache_dir(temp.string());

    test_pure_set_difference();

    ModelManager manager;
    test_full_pass_commits_and_recomputes(manager);
    test_incremental_pass_does_not_clobber(manager);

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All recipe hiding tests passed.\n");
    } else {
        std::printf("%d recipe hiding test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
