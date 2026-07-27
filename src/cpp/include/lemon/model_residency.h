#pragma once

#include <string>

#include "model_types.h"

namespace lemon {

// Why a model is being loaded. This is request-scoped intent, not a model
// capability and not a backend property.
enum class LoadPurpose {
    UserInference,
    RoutingDependency,
};

// Capacity/LRU class assigned to a live backend process. ModelType remains the
// deployment/API bucket (LLM, embedding, ...); ResidencyClass decides which
// count-based slot pool the process consumes.
enum class ResidencyClass {
    Standard,
    RoutingHelper,
};

inline ResidencyClass residency_class_for_load_purpose(LoadPurpose purpose) {
    return purpose == LoadPurpose::RoutingDependency
        ? ResidencyClass::RoutingHelper
        : ResidencyClass::Standard;
}

inline LoadPurpose load_purpose_for_residency_class(ResidencyClass residency_class) {
    return residency_class == ResidencyClass::RoutingHelper
        ? LoadPurpose::RoutingDependency
        : LoadPurpose::UserInference;
}

inline std::string residency_class_to_string(ResidencyClass residency_class) {
    switch (residency_class) {
        case ResidencyClass::Standard:
            return "standard";
        case ResidencyClass::RoutingHelper:
            return "routing_helper";
    }
    return "standard";
}

// Standard pools honor max_loaded_models. A RoutingHelper pool is scoped to
// one distinct helper model, so the per-pool limit stays one while multiple
// helper models required by a policy can remain resident together.
inline int residency_limit(ResidencyClass residency_class, int standard_limit) {
    return residency_class == ResidencyClass::RoutingHelper ? 1 : standard_limit;
}

// Standard capacity remains shared by ModelType. Routing-helper capacity is
// keyed by the distinct helper model, so a policy can keep multiple same-type
// helpers warm without making them compete for one type-wide slot.
inline bool same_residency_pool(ModelType lhs_type,
                                ResidencyClass lhs_class,
                                const std::string& lhs_model,
                                ModelType rhs_type,
                                ResidencyClass rhs_class,
                                const std::string& rhs_model) {
    if (lhs_class != rhs_class) {
        return false;
    }
    if (lhs_class == ResidencyClass::RoutingHelper) {
        return lhs_type == rhs_type && lhs_model == rhs_model;
    }
    return lhs_type == rhs_type;
}

// Internal routing work must not evict an already-resident process to acquire
// an exclusive hardware slot. Reject helper -> standard and helper -> helper
// displacement deterministically instead of creating cross-request NPU/FLM
// thrashing. A direct user request has precedence and may evict a helper via
// the backend's normal exclusivity policy.
inline bool should_reject_residency_displacement(
    ResidencyClass incoming,
    ResidencyClass resident) {
    (void)resident;
    return incoming == ResidencyClass::RoutingHelper;
}

inline std::string residency_pool_to_string(ModelType type,
                                            ResidencyClass residency_class) {
    return residency_class_to_string(residency_class) + "/" + model_type_to_string(type);
}

} // namespace lemon
