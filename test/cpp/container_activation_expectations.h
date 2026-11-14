#pragma once

#include "backend_definition_exports_internal.h"

#include <set>
#include <string>

namespace container_activation {

inline bool exact_policies() {
    const auto& policies = lemon::backends::embedded_v2_policies();
    std::set<std::string> leaves, owners;
    for (const auto& leaf : policies.leaves) leaves.insert(leaf.key.id);
    for (const auto& owner : policies.execution_owners) owners.insert(owner.key.id);
    return policies.leaves.size() == 8 && policies.execution_owners.size() == 6 &&
        leaves == std::set<std::string>{"llamacpp", "hrx", "gufo", "rocmfpx", "halogen", "halobox", "nathanw", "ds4_rocm"} &&
        owners == std::set<std::string>{"gufo", "rocmfpx", "halogen", "halobox", "nathanw", "ds4_rocm"};
}

inline nlohmann::json document(nlohmann::json staged) {
    for (auto& record : staged.at("merge").at("rationale")) {
        auto& text = record.at("text");
        if (text.get<std::string>().rfind("Inactive builtin leaf", 0) == 0)
            text = "Experimental container backend. Runtime and packaging qualification is recorded separately for each engine.";
    }
    return staged;
}

}
