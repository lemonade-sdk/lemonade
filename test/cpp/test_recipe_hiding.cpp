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

// The memory heuristic only sees models that already cleared the hardware/OS
// gate (check_recipe_supported) and only runs when system RAM is readable, so
// the stateful tests cannot assume any particular recipe reaches it. Probe for
// one that does: a tiny model must survive (recipe supported here) and a huge
// one must not (size filter actually active). Probes pass
// track_recipe_availability=false, so they never disturb the side table.
static std::string find_probe_recipe(ModelManager& manager) {
    for (const char* candidate : {"llamacpp", "whispercpp", "kokoro", "sd-cpp"}) {
        std::map<std::string, ModelInfo> tiny = {{"ProbeTiny", model(candidate, 0.01)}};
        if (manager.filter_models_by_backend(tiny, false).empty()) continue;

        std::map<std::string, ModelInfo> huge = {{"ProbeHuge", model(candidate, HUGE_GB)}};
        if (!manager.filter_models_by_backend(huge, false).empty()) continue;

        return candidate;
    }
    return "";
}

static void test_full_pass_commits_and_recomputes(ModelManager& manager,
                                                  const std::string& recipe) {
    // Every model of `recipe` is unfillable -> the recipe is recorded. This is
    // the positive half: without it the "unchanged" assertions below would also
    // hold if the full pass had committed nothing at all.
    std::map<std::string, ModelInfo> all_filtered = {{"Huge", model(recipe, HUGE_GB)}};
    manager.filter_models_by_backend(all_filtered, /*track_recipe_availability=*/true);
    check("full pass records a recipe whose every model was size-filtered",
          manager.recipes_all_models_filtered_snapshot().count(recipe) == 1);

    // One model still fits -> partial filtering, so the recipe must not be
    // recorded, and the recompute must drop the entry seeded above.
    std::map<std::string, ModelInfo> partial = {
        {"Huge", model(recipe, HUGE_GB)},
        {"Tiny", model(recipe, 0.01)},
    };
    manager.filter_models_by_backend(partial, /*track_recipe_availability=*/true);
    check("full pass recomputes: partially-filtered recipe is dropped",
          manager.recipes_all_models_filtered_snapshot().count(recipe) == 0);

    // Nothing filtered at all -> still not recorded.
    std::map<std::string, ModelInfo> all_fit = {{"Tiny", model(recipe, 0.01)}};
    manager.filter_models_by_backend(all_fit, /*track_recipe_availability=*/true);
    check("full pass never records a recipe that kept a visible model",
          manager.recipes_all_models_filtered_snapshot().count(recipe) == 0);
}

static void test_incremental_pass_does_not_clobber(ModelManager& manager,
                                                   const std::string& recipe) {
    // Seed the side table with a full pass, then run an incremental single-model
    // pass (as add_model_to_cache does). The incremental pass must leave the
    // side table untouched.
    std::map<std::string, ModelInfo> full = {{"Huge", model(recipe, HUGE_GB)}};
    manager.filter_models_by_backend(full, /*track_recipe_availability=*/true);
    std::set<std::string> seeded = manager.recipes_all_models_filtered_snapshot();

    check("seeded side table actually holds the all-filtered recipe",
          seeded.count(recipe) == 1);

    std::map<std::string, ModelInfo> one = {{"Tiny", model(recipe, 0.01)}};
    manager.filter_models_by_backend(one, /*track_recipe_availability=*/false);
    std::set<std::string> after_incremental = manager.recipes_all_models_filtered_snapshot();

    check("incremental pass leaves the availability side table unchanged",
          after_incremental == seeded);
    check("incremental pass does not drop the recorded recipe",
          after_incremental.count(recipe) == 1);
}

int main() {
    fs::path temp = fs::temp_directory_path() / "lemonade-recipe-hiding-test";
    fs::create_directories(temp);
    lemon::utils::set_cache_dir(temp.string());

    test_pure_set_difference();

    ModelManager manager;
    // A vacuous pass is exactly what this test guards against, so a host where
    // no recipe reaches the memory heuristic is reported as a failure rather
    // than skipped silently.
    std::string probe = find_probe_recipe(manager);
    if (probe.empty()) {
        check("found a recipe that reaches the memory heuristic on this host", false);
    } else {
        std::printf("Using probe recipe: %s\n", probe.c_str());
        test_full_pass_commits_and_recomputes(manager, probe);
        test_incremental_pass_does_not_clobber(manager, probe);
    }

    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All recipe hiding tests passed.\n");
    } else {
        std::printf("%d recipe hiding test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
