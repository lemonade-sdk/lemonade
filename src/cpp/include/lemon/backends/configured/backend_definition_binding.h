#pragma once

#include "lemon/backends/configured/backend_catalog_admission.h"
#include "lemon/backends/configured/backend_definition_execution.h"

namespace lemon::backends {

struct ExecutionBindLimits {
    std::size_t slots = 4096;
    std::size_t retained_bytes = 128 * 1024 * 1024;
    std::size_t work_visits = 1048576;
    std::size_t diagnostics = 100;
};
struct ExecutionBindMetrics {
    std::size_t slots = 0;
    std::size_t retained_bytes = 0;
    std::size_t work_visits = 0;
};
class DefinitionExecutionBinder;
namespace binding { struct Inspection; }
class ExecutableDefinition final {
public:
    const CompiledDefinition& compiled() const noexcept;
    const std::string& execution_id() const noexcept;
    const ExecutionBindMetrics& metrics() const noexcept;
private:
    ExecutableDefinition(const ExecutableDefinition&) = delete;
    ExecutableDefinition& operator=(const ExecutableDefinition&) = delete;
    struct Impl;
    explicit ExecutableDefinition(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> impl_;
    friend class DefinitionExecutionBinder;
    friend struct binding::Inspection;
};
struct ExecutionBindResult {
    std::shared_ptr<const ExecutableDefinition> definition;
    CompositionDiagnostics diagnostics;
};
ExecutionBindResult bind_execution_definition(
    std::shared_ptr<const AdmittedCatalog> admitted,
    std::shared_ptr<const CompiledDefinition> compiled,
    const ExecutionBindLimits& limits = {});

}
