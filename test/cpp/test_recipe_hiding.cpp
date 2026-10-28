// Tests for the rule that reports a backend as unavailable (so clients hide it
// from the recipe/backends listing) when every one of its built-in models was
// dropped by the system-memory heuristic. Two layers are covered:
//   * the pure set-difference at the heart of the rule, and
//   * a filtering pass run against the machine its caller declared.

#include "lemon/model_manager.h"
#include "lemon/models/store_builder.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace models = lemon::models;

using lemon::ModelInfo;
using lemon::RuntimeConfig;
using lemon::json;

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

static constexpr double HUGE_GB = 1.0e6;

// The machine these tests mean: RAM the heuristic can read, a pool a 0.01 GB model
// fits and a 1e6 GB one does not, and a recipe the machine can run. Declaring it is
// what keeps the test honest on a host with terabytes of RAM, and on one with no
// backend installed at all.
static lemon::models::BackendAvailability test_machine() {
    lemon::models::BackendAvailability availability;
    availability.system_ram_gb = 32.0;
    availability.largest_mem_pool_gb = 24.0;
    availability.processor = "Declared for the test";
    return availability;
}

// A machine the caller declares, in the form the filter asks for.
static lemon::models::AvailabilityProbe on_test_machine(
    lemon::models::BackendAvailability machine = test_machine()) {
    return [machine](const std::map<std::string, ModelInfo>&) { return machine; };
}

using Reasons = std::shared_ptr<const lemon::models::FilterReasons>;

static bool reports(const std::vector<std::string>& empty_recipes, const std::string& recipe) {
    return std::find(empty_recipes.begin(), empty_recipes.end(), recipe) != empty_recipes.end();
}

static void test_full_pass_recomputes(const std::string& recipe) {
    // Every model of `recipe` is unfillable -> the recipe is reported. This is
    // the positive half: without it the "not reported" assertions below would
    // also hold if the pass reported nothing at all.
    std::map<std::string, ModelInfo> all_filtered = {{"Huge", model(recipe, HUGE_GB)}};
    const Reasons reported = models::filter_for_host(all_filtered, on_test_machine()).reasons;
    check("full pass reports a recipe whose every model was size-filtered",
          reports(reported->empty_recipes, recipe));

    // One model still fits -> partial filtering, so the recipe is not reported
    // even though the pass above reported it: each pass recomputes from scratch.
    std::map<std::string, ModelInfo> partial = {
        {"Huge", model(recipe, HUGE_GB)},
        {"Tiny", model(recipe, 0.01)},
    };
    check("a pass that replaces the table drops a recipe with a visible model",
          !reports(models::filter_for_host(partial, on_test_machine()).reasons->empty_recipes, recipe));
}

static void test_the_declared_machine_decides() {
    std::map<std::string, ModelInfo> huge = {{"Huge", model("llamacpp", HUGE_GB)}};
    check("the declared pool decides a model no machine can hold",
          models::filter_for_host(huge, on_test_machine()).visible.empty());

    auto switched_off = test_machine();
    switched_off.filtering_disabled = true;
    check("filtering off hides nothing even from a pool that cannot hold it",
          models::filter_for_host(huge, on_test_machine(switched_off)).visible.count("Huge") == 1);

    auto without_backend = test_machine();
    without_backend.recipes["llamacpp"] =
        lemon::models::RecipeAvailability{"no backend here", "", false};
    check("a recipe the declared machine cannot run hides its model",
          models::filter_for_host(std::map<std::string, ModelInfo>{{"Tiny", model("llamacpp", 0.01)}},
                                  on_test_machine(without_backend))
              .visible.empty());

    // A recipe this host probably cannot run, listed because the declared machine can.
    check("a recipe this machine may not run shows when the declared one does",
          models::filter_for_host(std::map<std::string, ModelInfo>{{"NPU", model("ryzenai-llm", 0.01)}},
                                  on_test_machine())
              .visible.count("NPU") == 1);
}

static void test_the_operator_switch_outranks_a_declared_machine() {
    RuntimeConfig config(json{{"disable_model_filtering", true}});
    RuntimeConfig::set_global(&config);
    check("filtering off outranks the declared pool",
          models::filter_for_host(std::map<std::string, ModelInfo>{{"Huge", model("llamacpp", HUGE_GB)}},
                                  on_test_machine())
              .visible.count("Huge") == 1);
    RuntimeConfig::set_global(nullptr);
}

int main() {
    test_pure_set_difference();

    test_full_pass_recomputes("llamacpp");
    test_the_declared_machine_decides();
    test_the_operator_switch_outranks_a_declared_machine();

    if (g_failures == 0) {
        std::printf("All recipe hiding tests passed.\n");
    } else {
        std::printf("%d recipe hiding test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
