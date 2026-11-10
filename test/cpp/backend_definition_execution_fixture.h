#pragma once

#include "backend_definition_execution_internal.h"
#include "backend_definition_export_fixture.h"

#include <cstdio>

namespace execution_fixture {
using namespace export_fixture;
using namespace lemon;
namespace ex = lemon::backends::execution;
inline int checks = 0, failures = 0;
inline void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label.c_str()); std::fflush(stdout); }
}
inline Json fixture() { return Json::parse(read_bytes("execution/hook-free.json")); }
inline std::shared_ptr<const AuthorizedDefinition> authorize(const Json& document) {
    const auto bytes = document.dump();
    const auto id = document["id"].get<std::string>();
    const auto frozen = load_v2_authority_snapshot({{{DefinitionNamespace::External, id, 0}, bytes, "/PRIVATE_PATH"}});
    if (!frozen.snapshot) throw std::runtime_error("Fixture snapshot rejected");
    const auto result = DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, id, 0});
    if (!result.definition) {
        for (const auto& error : result.diagnostics.entries) std::printf("AUTH %s %s\n", error.code.c_str(), error.pointer.c_str());
        throw std::runtime_error("Fixture authorization rejected");
    }
    return result.definition;
}
inline ExecutionCompileResult compile(const Json& document, const ExecutionCompileLimits& limits = {}) {
    return compile_execution_definition(authorize(document), limits);
}
inline bool code(const ExecutionCompileResult& result, const std::string& expected) {
    return !result.definition && std::any_of(result.diagnostics.entries.begin(), result.diagnostics.entries.end(),
        [&](const auto& diagnostic) { return diagnostic.code == expected; });
}
inline const ex::Program& program(const CompiledDefinition& value) { return ex::Inspection::program(value); }
inline Json option(std::string id = "engine", std::string name = "fork_backend", Json value = "cpu", std::string type = "string") {
    return {{"id", id}, {"name", name}, {"value_type", type}, {"default_value", value}};
}
inline Json expression_document(const Json& value) {
    auto result = fixture();
    result["merge"]["operations"][0]["request"]["set"]["/answer"] = value;
    return result;
}
inline std::optional<Json> evaluate(const Json& value, const ex::InvocationFrame& frame = {}) {
    const auto result = compile(expression_document(value));
    if (!result.definition) return {};
    const auto& ir = program(*result.definition);
    return ir.expressions.at("/operations/0/request/set/~1answer").evaluate(frame, ir.bindings);
}
inline int finish() { std::printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0; }
}
