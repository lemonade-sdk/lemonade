#pragma once

#include "lemon/backend_operation.h"
#include "lemon/backends/configured/backend_definition_authorization.h"

namespace lemon::backends {

struct ExecutionCompileLimits {
    std::size_t normalized_bytes = 2 * 1024 * 1024;
    std::size_t normalized_nodes = 131072;
    std::size_t expression_nodes = 4096;
    std::size_t expression_depth = 32;
    std::size_t work_bytes = 16 * 1024 * 1024;
    std::size_t work_visits = 1048576;
    std::size_t retained_bytes = 128 * 1024 * 1024;
    std::size_t diagnostics = 100;
};

struct OptionBinding {
    std::string id;
    std::string public_name;
    std::string config_key;
    std::string type;
    nlohmann::json contract;
};

struct ExecutionCompileMetrics {
    std::size_t normalized_bytes = 0;
    std::size_t normalized_nodes = 0;
    std::size_t expression_nodes = 0;
    std::size_t retained_bytes = 0;
    std::size_t work_bytes = 0;
    std::size_t work_visits = 0;
};

class DefinitionExecutionCompiler;
namespace execution { struct Inspection; }

class CompiledDefinition final {
public:
    const AuthorizedDefinition& authorized() const noexcept;
    const nlohmann::json& normalized_declaration() const noexcept;
    const std::vector<OptionBinding>& option_bindings() const noexcept;
    const std::vector<OperationDescription>& operation_descriptions() const noexcept;
    const std::string& compilation_id() const noexcept;
    const ExecutionCompileMetrics& metrics() const noexcept;

private:
    struct Impl;
    explicit CompiledDefinition(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
    friend class DefinitionExecutionCompiler;
    friend struct execution::Inspection;
};

struct ExecutionCompileResult {
    std::shared_ptr<const CompiledDefinition> definition;
    CompositionDiagnostics diagnostics;
};

ExecutionCompileResult compile_execution_definition(
    std::shared_ptr<const AuthorizedDefinition> authorized,
    const ExecutionCompileLimits& limits = {});

}
