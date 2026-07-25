#include "lemon/backends/llamacpp/llamacpp_request.h"

#include <cstdint>
#include <string>

namespace lemon {
namespace backends {
namespace llamacpp {
namespace {

constexpr std::uint64_t GRAMMAR_REPETITION_LIMIT = 2000;

bool exceeds_grammar_repetition_limit(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() >= GRAMMAR_REPETITION_LIMIT;
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() >= static_cast<std::int64_t>(GRAMMAR_REPETITION_LIMIT);
    }
    return false;
}

void sanitize_schema(nlohmann::json& schema) {
    if (schema.is_array()) {
        for (auto& value : schema) {
            sanitize_schema(value);
        }
        return;
    }
    if (!schema.is_object()) return;

    // llama.cpp rejects grammar repetitions at this threshold. Remove oversized
    // upper bounds instead of clamping them, which would reject valid tool arguments.
    // Remove this workaround once ggml-org/llama.cpp#17473 is fixed in pinned builds.
    for (const std::string key : {"maxLength", "maxItems"}) {
        auto limit = schema.find(key);
        if (limit != schema.end() && exceeds_grammar_repetition_limit(*limit)) {
            schema.erase(limit);
        }
    }

    for (auto& item : schema.items()) {
        sanitize_schema(item.value());
    }
}

}  // namespace

nlohmann::json sanitize_tool_schema_limits(nlohmann::json request) {
    auto tools = request.find("tools");
    if (tools == request.end() || !tools->is_array()) return request;

    for (auto& tool : *tools) {
        if (!tool.is_object()) continue;

        auto parameters = tool.find("parameters");
        if (parameters != tool.end()) {
            sanitize_schema(*parameters);
        }

        auto function = tool.find("function");
        if (function == tool.end() || !function->is_object()) continue;

        parameters = function->find("parameters");
        if (parameters != function->end()) {
            sanitize_schema(*parameters);
        }
    }

    return request;
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
