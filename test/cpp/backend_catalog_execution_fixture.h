#pragma once

#include "catalog_admission_fixture.h"
#include "lemon/backends/configured/backend_catalog_execution.h"

namespace catalog_execution_fixture {
using namespace admission_fixture;

inline Json executable_root(const std::string& id = "catalog-root") {
    auto value = Json::parse(read_bytes("execution/hook-free.json"));
    value["id"] = value["recipe"] = id;
    return value;
}
inline CatalogBuildResult build(const std::vector<Json>& values, const CatalogExecutionLimits& limits = {},
    std::shared_ptr<const FrozenBuiltinCatalog> builtins = {}) {
    std::vector<std::string> bytes;
    for (const auto& value : values) bytes.push_back(value.dump());
    std::vector<ExternalDefinitionInput> inputs;
    for (const auto& value : bytes) inputs.push_back({value, ""});
    return build_backend_catalog(builtins ? std::move(builtins) : freeze_builtin_catalog(), inputs, limits);
}
inline void dump_errors(const CatalogBuildResult& value) {
    for (const auto& row : value.diagnostics.entries) std::printf("  %s %s %s\n", row.code.c_str(), row.definition.c_str(), row.pointer.c_str());
}
}
