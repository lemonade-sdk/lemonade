// Standalone contract test for model residency roles and slot-pool limits.
// Compile manually with:
//   g++ -std=c++17 -I src/cpp/include test/cpp/test_model_residency.cpp -o test_model_residency

#include "lemon/model_residency.h"

#include <cstdio>

using lemon::LoadPurpose;
using lemon::ModelType;
using lemon::ResidencyClass;
using lemon::load_purpose_for_residency_class;
using lemon::residency_class_for_load_purpose;
using lemon::residency_class_to_string;
using lemon::residency_limit;
using lemon::residency_pool_to_string;
using lemon::same_residency_pool;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

int main() {
    check("user inference maps to standard",
          residency_class_for_load_purpose(LoadPurpose::UserInference) ==
              ResidencyClass::Standard);
    check("routing dependency maps to routing helper",
          residency_class_for_load_purpose(LoadPurpose::RoutingDependency) ==
              ResidencyClass::RoutingHelper);
    check("routing helper round-trips to routing dependency",
          load_purpose_for_residency_class(ResidencyClass::RoutingHelper) ==
              LoadPurpose::RoutingDependency);

    check("standard pool honors configured limit",
          residency_limit(ResidencyClass::Standard, 3) == 3);
    check("standard pool preserves unlimited",
          residency_limit(ResidencyClass::Standard, -1) == -1);
    check("routing helper has one slot independent of standard limit",
          residency_limit(ResidencyClass::RoutingHelper, -1) == 1 &&
              residency_limit(ResidencyClass::RoutingHelper, 8) == 1);

    check("same type and role share a pool",
          same_residency_pool(ModelType::LLM, ResidencyClass::Standard,
                              ModelType::LLM, ResidencyClass::Standard));
    check("same type but different role do not share a pool",
          !same_residency_pool(ModelType::LLM, ResidencyClass::Standard,
                               ModelType::LLM, ResidencyClass::RoutingHelper));
    check("different types do not share a helper pool",
          !same_residency_pool(ModelType::LLM, ResidencyClass::RoutingHelper,
                               ModelType::EMBEDDING, ResidencyClass::RoutingHelper));

    check("health class string is stable",
          residency_class_to_string(ResidencyClass::RoutingHelper) ==
              "routing_helper");
    check("pool string contains role and type",
          residency_pool_to_string(ModelType::LLM,
                                   ResidencyClass::RoutingHelper) ==
              "routing_helper/llm");

    if (failures == 0) {
        std::printf("\nAll model_residency tests passed\n");
        return 0;
    }
    std::printf("\n%d model_residency test(s) FAILED\n", failures);
    return 1;
}
