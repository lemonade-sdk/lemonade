#pragma once

#include "lemon/backend_operation.h"
#include "lemon/recipe_options.h"

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon::backends {

enum class DefinitionOrigin { Builtin, External };
struct DefinitionSource {
    std::string file;
    std::string text;
    DefinitionOrigin origin;
};
struct DefinitionDiagnostic {
    std::string code, definition, file, pointer, message;
};
struct Expression {
    enum class Kind { Literal, Ref, Concat, String, Coalesce, Present, Eq, Gt, All, Any, Not };
    Kind kind = Kind::Literal;
    json literal;
    std::string reference, type;
    std::optional<json> default_value;
    std::vector<Expression> children;
    std::optional<json> evaluate(const json& context) const;
};
struct FormatMetadata {
    std::string content_type;
    std::map<std::string, std::string> headers;
};
struct OperationProgram {
    OperationDescription description;
    std::string kind, method, path, port, codec;
    long timeout_seconds = 0;
    std::vector<std::string> formats;
    std::map<std::string, FormatMetadata> format_metadata;
    bool identity = false;
    bool omit_missing = false;
    std::vector<std::string> remove;
    std::vector<std::pair<std::string, Expression>> set;
    std::vector<std::string> request_hooks, response_hooks;
};
struct BackendDefinition {
    std::string id, recipe, digest;
    DefinitionOrigin origin;
    json records;
    std::map<std::string, Expression> expressions;
    std::vector<OperationProgram> operations;
    std::optional<json> evaluate(const std::string& pointer, const json& context) const;
    json required_value(const std::string& pointer, const json& context) const;
};
struct DefinitionValidationResult {
    std::shared_ptr<const BackendDefinition> definition;
    std::vector<DefinitionDiagnostic> diagnostics;
};
class DefinitionValidationError : public std::runtime_error {
public:
    explicit DefinitionValidationError(std::vector<DefinitionDiagnostic> errors)
        : std::runtime_error("Invalid backend definition"), diagnostics(std::move(errors)) {}
    const std::vector<DefinitionDiagnostic> diagnostics;
};

}
