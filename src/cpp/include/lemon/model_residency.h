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

// Router/classifier dependencies get one warm process per ModelType. Keeping
// this independent from max_loaded_models prevents a normal LLM candidate from
// evicting an LLM router when the user keeps the default standard limit of 1.
inline int residency_limit(ResidencyClass residency_class, int standard_limit) {
    return residency_class == ResidencyClass::RoutingHelper ? 1 : standard_limit;
}

inline bool same_residency_pool(ModelType lhs_type,
                                ResidencyClass lhs_class,
                                ModelType rhs_type,
                                ResidencyClass rhs_class) {
    return lhs_type == rhs_type && lhs_class == rhs_class;
}

inline std::string residency_pool_to_string(ModelType type,
                                            ResidencyClass residency_class) {
    return residency_class_to_string(residency_class) + "/" + model_type_to_string(type);
}

} // namespace lemon
