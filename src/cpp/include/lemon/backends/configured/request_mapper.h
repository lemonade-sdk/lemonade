#pragma once

#include "lemon/backends/configured/backend_definition.h"

#include <string_view>

namespace lemon::backends {

struct MappingContext { json values = json::object(); };
struct MappedRequest {
    const OperationProgram* operation;
    json body;
    std::optional<std::string> original_body;
};
class RequestMapper {
public:
    MappedRequest map(const OperationProgram& operation, const MappingContext& context,
                     const json& request, std::optional<std::string_view> original_body = std::nullopt) const;
};

}
