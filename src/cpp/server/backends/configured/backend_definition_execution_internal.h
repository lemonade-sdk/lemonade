#pragma once

#include "backend_definition_v2_internal.h"
#include "backend_hook_contracts_internal.h"
#include "lemon/backends/configured/backend_definition_execution.h"

#include <map>

namespace lemon::backends::execution {

using Json = nlohmann::json;

inline constexpr unsigned compiler_revision = 2;
inline constexpr unsigned defaults_revision = 1;
inline constexpr unsigned binding_revision = 1;
inline constexpr unsigned adapter_revision = 1;

struct Failure {
    std::string code;
    std::string pointer;
};

struct Budget {
    ExecutionCompileLimits limits;
    ExecutionCompileMetrics metrics;
    void work(std::size_t bytes, std::size_t visits = 1);
    void keep(std::size_t bytes);
    void copy(const Json& value);
};

struct Counters {
    std::size_t declaration_copies = 0;
};
extern thread_local Counters counters;

struct InvocationFrame {
    Json values = Json::object();
    Json options = Json::object();
};

struct Expression {
    enum class Kind { Literal, Ref, Option, Concat, String, Coalesce, Present, Eq, Gt,
        All, Any, Not, When, ParentPath, PathJoin };
    Kind kind = Kind::Literal;
    Json literal;
    std::string reference;
    std::string type;
    std::optional<Json> default_value;
    std::size_t slot = 0;
    std::vector<Expression> children;
    std::optional<Json> evaluate(const InvocationFrame& frame, const std::vector<OptionBinding>& bindings) const;
};

struct Argument {
    std::string id;
    std::optional<std::string> flag;
    std::string reserve;
    std::vector<std::string> aliases;
    std::string condition;
    std::vector<std::string> values;
};

struct CustomDefault {
    std::string id;
    std::string args;
    std::string flag;
    std::string stage;
    std::string condition;
    bool respect_merge_args = true;
    std::vector<std::string> aliases;
};

struct Port {
    std::string name;
    std::string strategy;
    bool reject_zero = false;
    std::string error;
    unsigned span = 1;
    unsigned attempts = 1;
    std::string base;
    unsigned offset = 0;
};

struct InstallCase {
    std::string id;
    std::string repo;
    std::map<std::string, std::vector<std::string>> selectors;
    std::string filename;
    std::optional<std::string> version_override;
    std::optional<std::string> sha256;
};

struct InstallProgram {
    std::string strategy;
    std::vector<InstallCase> cases;
    Json records;
};

struct ArtifactProgram {
    std::string name;
    std::string role;
    std::string required_when;
    std::string missing_error;
    Json records;
};

struct EnvironmentAssignment {
    std::string name;
    std::string expression;
};

struct LaunchProgram {
    std::vector<std::string> prepare_order;
    std::vector<Port> ports;
    std::string program_source;
    std::string install_policy;
    Json fixed_backend;
    std::string environment_preset;
    std::string environment_order;
    std::vector<EnvironmentAssignment> environment_set;
    std::optional<std::size_t> backend_slot;
    std::optional<std::size_t> custom_args_slot;
    std::vector<Argument> argv;
    std::vector<CustomDefault> defaults;
    std::map<ModelType, std::uint64_t> context_floor;
    std::map<std::string, std::string> working_directory;
    Json records;
};

struct OperationProgram {
    OperationDescription description;
    std::string id;
    std::string kind;
    std::string method;
    std::string path;
    std::string port;
    std::string codec;
    std::uint64_t timeout_seconds = 0;
    bool identity = false;
    bool omit_missing = false;
    std::vector<std::string> remove;
    std::map<std::string, std::string> set;
};

struct Program {
    Json normalized;
    std::vector<OptionBinding> bindings;
    std::map<std::string, std::size_t> slots;
    std::map<std::string, Expression> expressions;
    LaunchProgram launch;
    InstallProgram install;
    std::vector<ArtifactProgram> artifacts;
    std::vector<OperationProgram> operations;
    std::vector<OperationDescription> descriptions;
};

using SchemaVisitor = std::function<void(const Json&, const std::string&, const std::string&)>;
void schema_walk(const Json& value, const Json& rule, const std::string& pointer,
    Budget& budget, const SchemaVisitor& visitor);
void bind_options(Program& program, Budget& budget);
void compile_expressions(Program& program, Budget& budget);
void compile_programs(Program& program, Budget& budget);
std::string compilation_identity(const ResolvedDefinition& source, const Program& program,
    Budget& budget, unsigned revision = compiler_revision);
ModelType model_type(const std::string& name);
bool type_matches(const Json& value, const std::string& type);
void require(bool condition, const std::string& code, const std::string& pointer);

struct Inspection {
    static const Program& program(const CompiledDefinition& definition);
};

}
