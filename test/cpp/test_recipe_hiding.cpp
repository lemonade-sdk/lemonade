// Tests for the rule that hides a backend from the recipe/backends listing
// when every one of its built-in models was dropped by the system-memory
// heuristic. The set-difference at the heart of the rule is pure and
// hardware-independent, so it is exercised directly here; the end-to-end wiring
// (size filtering -> recipes_with_all_models_filtered() -> handle_system_info
// erase, with dynamic-model backends exempt) is covered by manual server runs.

#include "lemon/model_manager.h"

#include <cstdio>
#include <set>
#include <string>

using lemon::ModelManager;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static bool eq(const std::set<std::string>& a, const std::set<std::string>& b) {
    return a == b;
}

int main() {
    using S = std::set<std::string>;

    // A recipe whose only models were all size-filtered, with nothing visible,
    // is hidden.
    check("all built-ins filtered -> recipe hidden",
          eq(ModelManager::recipes_missing_all_models({"ds4"}, {}),
             S{"ds4"}));

    // A recipe with at least one model still visible is kept, even if some of
    // its models were filtered (partial filtering).
    check("partial filtering -> recipe kept",
          eq(ModelManager::recipes_missing_all_models({"llamacpp"}, {"llamacpp"}),
             S{}));

    // A recipe that never had a model filtered is not a hide candidate.
    check("no filtered models -> nothing hidden",
          eq(ModelManager::recipes_missing_all_models({}, {"llamacpp", "kokoro"}),
             S{}));

    // Mixed: one recipe fully filtered, another partially, a third untouched.
    check("mixed recipes resolve independently",
          eq(ModelManager::recipes_missing_all_models(
                 {"ds4", "vllm"}, {"vllm", "llamacpp"}),
             S{"ds4"}));

    // Only the size-filtered set drives hiding: a recipe absent from it is never
    // hidden regardless of visibility.
    check("recipe not size-filtered is never hidden",
          eq(ModelManager::recipes_missing_all_models({"ds4"}, {"ds4", "llamacpp"}),
             S{}));

    if (g_failures == 0) {
        std::printf("All recipe hiding tests passed.\n");
    } else {
        std::printf("%d recipe hiding test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
