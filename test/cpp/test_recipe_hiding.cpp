// Tests for the rule that reports a backend as unavailable (so clients hide it
// from the recipe/backends listing) when every one of its built-in models was
// dropped by the system-memory heuristic. Two layers are covered:
//   * the pure set-difference at the heart of the rule, and
//   * the contract that a pass over part of the registry carries the availability
//     side table instead of reducing it to that part.

#include "lemon/model_manager.h"
#include "lemon/models/store_builder.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
          lemon::models::recipes_missing_all_models({"ds4"}, {}) == S{"ds4"});

    check("partial filtering -> recipe kept",
          lemon::models::recipes_missing_all_models({"llamacpp"}, {"llamacpp"}) == S{});

    check("no filtered models -> nothing reported",
          lemon::models::recipes_missing_all_models({}, {"llamacpp", "kokoro"}) == S{});

    check("mixed recipes resolve independently",
          lemon::models::recipes_missing_all_models({"ds4", "vllm"}, {"vllm", "llamacpp"})
              == S{"ds4"});

    check("recipe not size-filtered is never reported",
          lemon::models::recipes_missing_all_models({"ds4"}, {"ds4", "llamacpp"}) == S{});
}

// A size so large no real machine can hold it, so the model is always
// memory-filtered regardless of where the test runs.
static constexpr double HUGE_GB = 1.0e6;

// The memory heuristic only sees models that already cleared the hardware/OS
// gate (check_recipe_supported) and only runs when system RAM is readable, so
// these tests cannot assume any particular recipe reaches it. Probe for one that
// does: a tiny model must survive (recipe supported here) and a huge one must
// not (size filter actually active).
static std::string find_probe_recipe(ModelManager& manager) {
    for (const char* candidate : {"llamacpp", "whispercpp", "kokoro", "sd-cpp"}) {
        std::map<std::string, ModelInfo> tiny = {{"ProbeTiny", model(candidate, 0.01)}};
        if (manager.filter_for_host(tiny).visible.empty()) continue;

        std::map<std::string, ModelInfo> huge = {{"ProbeHuge", model(candidate, HUGE_GB)}};
        if (!manager.filter_for_host(huge).visible.empty()) continue;

        return candidate;
    }
    return "";
}

using Reasons = std::shared_ptr<const lemon::models::FilterReasons>;

static bool reports(const std::vector<std::string>& empty_recipes, const std::string& recipe) {
    return std::find(empty_recipes.begin(), empty_recipes.end(), recipe) != empty_recipes.end();
}

static void test_full_pass_recomputes(ModelManager& manager, const std::string& recipe) {
    // Every model of `recipe` is unfillable -> the recipe is reported. This is
    // the positive half: without it the "not reported" assertions below would
    // also hold if the pass reported nothing at all.
    std::map<std::string, ModelInfo> all_filtered = {{"Huge", model(recipe, HUGE_GB)}};
    const Reasons reported = manager.filter_for_host(all_filtered).reasons;
    check("full pass reports a recipe whose every model was size-filtered",
          reports(reported->empty_recipes, recipe));

    // One model still fits -> partial filtering, so the recipe is not reported
    // even though the pass above reported it: each pass recomputes from scratch.
    std::map<std::string, ModelInfo> partial = {
        {"Huge", model(recipe, HUGE_GB)},
        {"Tiny", model(recipe, 0.01)},
    };
    check("a pass that replaces the table drops a recipe with a visible model",
          !reports(manager.filter_for_host(partial).reasons->empty_recipes, recipe));
}

int main() {
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
        test_full_pass_recomputes(manager, probe);
    }

    if (g_failures == 0) {
        std::printf("All recipe hiding tests passed.\n");
    } else {
        std::printf("%d recipe hiding test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
