#include "backend_builtin_seeds_internal.h"
#include "backend_factories_generated.h"

namespace lemon::backends {

const std::vector<BuiltinRegistrationSeed>& builtin_registration_seeds() {
    static const auto seeds = generated_builtin_seeds();
    return seeds;
}

}
