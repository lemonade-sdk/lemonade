#pragma once

#include "backend_definition_execution_internal.h"
#include "backend_typed_hook_registry_internal.h"
#include "lemon/backends/configured/backend_definition_binding.h"

namespace lemon::backends { class DefinitionRunner; }

namespace lemon::backends::binding {

inline constexpr unsigned binder_revision = 1;
struct BoundHook {
    std::size_t symbolic_index;
    typed::Target target;
};
ExecutionBindLimits bounded(ExecutionBindLimits limits);
struct Budget {
    ExecutionBindLimits limits;
    ExecutionBindMetrics metrics;
    void keep(std::size_t bytes);
    void work(std::size_t visits);
    void slots(std::size_t count);
};
std::string execution_identity(const AdmittedCatalog& admitted, const CompiledDefinition& compiled,
    Budget& budget, unsigned revision = binder_revision);
struct Inspection {
#ifndef LEMONADE_BINDING_TEST
private:
    friend class ::lemon::backends::DefinitionRunner;
#endif
    static const std::vector<BoundHook>& hooks(const ExecutableDefinition& definition);
    static const AdmittedCatalog& admitted(const ExecutableDefinition& definition);
};
#ifdef LEMONADE_BINDING_TEST
struct Counters {
    std::size_t membership_scans = 0;
    std::size_t target_checks = 0;
    std::size_t slot_copies = 0;
    std::size_t identities = 0;
};
extern thread_local Counters counters;
extern thread_local const std::vector<typed::Registration>* test_targets;
#endif

}
