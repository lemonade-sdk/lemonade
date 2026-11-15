#include "lemon/server/hybrid_slot_scope.h"
#include <iostream>
#include <string>

using namespace lemon::hybrid_slot_scope;

int main() {
    const std::string model_a = "hybrid_test_model_a";
    const std::string model_b = "hybrid_test_model_b";
    int failures = 0;
    auto check = [&](bool ok, const char* description) {
        std::cout << (ok ? "PASS " : "FAIL ") << description << '\n';
        failures += !ok;
    };

    check(resolved_slot() == -1, "ordinary requests have no private slot");
    note_ordinary_inference(model_a, true);
    check(ordinary_generation(model_a) == 1,
          "ordinary inference invalidates a model's private KV lease");
    check(ordinary_generation(model_b) == 0, "other model lease is unchanged");
    {
        Role outer(0);
        check(resolved_slot() == 0, "orchestrator uses private slot zero");
        note_ordinary_inference(model_a, true);
        check(ordinary_generation(model_a) == 1,
              "hybrid inference does not invalidate its own KV lease");
        {
            Role inner(1);
            check(resolved_slot() == 1, "worker uses private slot one");
        }
        check(resolved_slot() == 0, "nested role scope restores orchestrator");
    }
    check(resolved_slot() == -1, "role scope restores ordinary behavior");

    {
        InferenceGuard first(model_a);
        InferenceGuard second(model_b);
        check(true, "different model gates can be held simultaneously");
    }
    note_ordinary_inference(model_a, false);
    check(ordinary_generation(model_a) == 1,
          "models without Hybrid do not mutate the lease generation");
    return failures ? 1 : 0;
}
