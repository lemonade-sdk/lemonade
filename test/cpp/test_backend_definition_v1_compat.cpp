#include "backend_definition_resource_files.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/configured/backend_definition_composition.h"
#include "lemon/backends/configured/backend_definition_store.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <type_traits>

using namespace lemon;
using namespace lemon::backends;
using Json = nlohmann::json;

static_assert(!std::is_convertible<ResolvedDefinition, BackendDefinition>::value);

namespace {
Json expression_data(const Expression& expression) {
    Json children = Json::array();
    for (const auto& child : expression.children) children.push_back(expression_data(child));
    return {{"kind", static_cast<int>(expression.kind)}, {"literal", expression.literal},
        {"reference", expression.reference}, {"type", expression.type},
        {"has_default", expression.default_value.has_value()},
        {"default", expression.default_value.value_or(Json())}, {"children", children}};
}
Json diagnostics_data(const DefinitionValidationResult& result) {
    Json output = Json::array();
    for (const auto& diagnostic : result.diagnostics) output.push_back({
        {"code", diagnostic.code}, {"definition", diagnostic.definition},
        {"pointer", diagnostic.pointer}, {"message", diagnostic.message}});
    return output;
}
Json snapshot() {
    Json result = {{"source_sha", "410b5b4e9f9c7bdfcd1031ea6911795a8cce09ae"},
        {"features", installed_definition_features()}, {"definitions", Json::object()}};
    for (const auto& resource : k_embedded_resource_files) {
        const std::string path(resource.path);
        if (path.find("/resources/backends/") == std::string::npos) continue;
        const auto validated = BackendDefinitionStore().validate({"", resource.data, DefinitionOrigin::Builtin});
        if (!validated.definition || !validated.diagnostics.empty()) throw std::runtime_error("Invalid baseline definition");
        const auto& definition = *validated.definition;
        Json expressions = Json::object(), operations = Json::array();
        for (const auto& [key, value] : definition.expressions) expressions[key] = expression_data(value);
        for (const auto& operation : definition.operations) {
            Json set = Json::array(), formats = Json::object();
            for (const auto& [key, value] : operation.set) set.push_back({key, expression_data(value)});
            for (const auto& [key, value] : operation.format_metadata) formats[key] = {{"content_type", value.content_type}, {"headers", value.headers}};
            operations.push_back({{"operation", static_cast<int>(operation.description.operation)},
                {"delivery", static_cast<int>(operation.description.delivery)}, {"modes", operation.description.model_types},
                {"provenance", static_cast<int>(operation.description.provenance)}, {"kind", operation.kind},
                {"method", operation.method}, {"path", operation.path}, {"port", operation.port},
                {"codec", operation.codec}, {"handler", operation.handler}, {"timeout", operation.timeout_seconds},
                {"formats", operation.formats}, {"format_metadata", formats}, {"identity", operation.identity},
                {"omit_missing", operation.omit_missing}, {"remove", operation.remove}, {"set", set},
                {"request_hooks", operation.request_hooks}, {"response_hooks", operation.response_hooks}});
        }
        Json rejected = Json::object();
        const auto original = Json::parse(resource.data);
        for (const auto* mutation : {"version", "unknown", "required", "duplicate", "external"}) {
            auto value = original;
            if (std::string(mutation) == "version") value["schema_version"] = 2;
            if (std::string(mutation) == "unknown") value["unknown~/key"] = true;
            if (std::string(mutation) == "required") value.erase("launch");
            auto bytes = value.dump();
            if (std::string(mutation) == "duplicate") bytes.insert(1, "\"id\":\"duplicate\",");
            rejected[mutation] = diagnostics_data(BackendDefinitionStore().validate({"", bytes,
                std::string(mutation) == "external" ? DefinitionOrigin::External : DefinitionOrigin::Builtin}));
        }
        result["definitions"][definition.id] = {{"digest", definition.digest},
            {"source_bytes", schema_digest(resource.data)}, {"records", schema_digest(definition.records.dump())},
            {"expressions", schema_digest(expressions.dump())}, {"programs", schema_digest(operations.dump())},
            {"diagnostics", rejected}};
    }
    return result;
}
}
int main(int argc, char** argv) {
    try {
        const auto actual = snapshot();
        if (argc == 2 && std::string(argv[1]) == "--capture") { std::cout << actual.dump(2) << '\n'; return 0; }
        std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/backend-v1-compat/compiled.json");
        const auto expected = Json::parse(input);
        if (actual != expected) {
            std::cout << Json::diff(expected, actual).dump(2) << '\n';
            return 1;
        }
        if (actual["definitions"].size() != 8 || actual["features"]["schema"] != "sha256:43c4c93ac73f3cc88d9773c2ad55cdadc83f6497987c9ca8b449ceef4efff818") return 1;
        std::puts("8/8 v1 compiled definitions, programs, expression trees, features and diagnostics unchanged");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
