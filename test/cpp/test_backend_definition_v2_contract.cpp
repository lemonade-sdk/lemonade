#include "backend_definition_v2_internal.h"

#include <cstdio>
#include <fstream>
#include <limits>

using namespace lemon::backends;
using Json = nlohmann::json;
namespace {
int checks = 0;
int failures = 0;
void check(bool valid, const std::string& label) {
    ++checks;
    if (!valid) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
Json read(const std::string& name) {
    std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/" + name);
    return Json::parse(input);
}
bool valid(const Json& value, DefinitionV2SchemaRoot root) {
    const auto result = validate_v2_structure(value, root);
    return result.entries.empty() && !result.truncated;
}
}
int main() {
    const auto envelope = DefinitionV2SchemaRoot::Envelope;
    const auto declaration = DefinitionV2SchemaRoot::Declaration;
    for (const auto& fixture : read("contract.json")) {
        check(valid(fixture["value"], envelope) == fixture["valid"].get<bool>(), fixture["name"]);
    }
    check(v2::check_schema_keywords(v2::schema_document()).entries.empty(), "every shipped validation keyword is implemented");
    for (const auto* keyword : {"allOf", "not", "if", "propertyNames", "unevaluatedProperties", "unknown"}) {
        auto schema = v2::schema_document();
        schema["$defs"]["Option"][keyword] = Json::object();
        const auto errors = v2::check_schema_keywords(schema);
        check(errors.entries.size() == 1 && errors.entries[0].code == "unsupported_schema_keyword", std::string("detect ignored keyword ") + keyword);
    }
    auto schema = v2::schema_document();
    schema["$defs"]["Identifier"]["pattern"] = "(a+)+";
    check(v2::check_schema_keywords(schema).entries[0].code == "unsupported_schema_pattern", "unbounded schema patterns rejected by audit");
    schema = v2::schema_document();
    schema["$defs"]["Option"]["properties"]["id"]["$ref"] = "other.json#/$defs/Identifier";
    check(!v2::check_schema_keywords(schema).entries.empty(), "cross-file references fail keyword audit");
    auto base = read("llamacpp.example.json");
    check(valid(base, envelope), "llamacpp envelope");
    check(valid(read("hrx.example.json"), envelope), "hrx envelope");
    auto effective = base["merge"];
    for (const auto* key : {"schema_version", "id", "recipe"}) effective[key] = base[key];
    check(valid(effective, declaration), "complete llama declaration");
    check(!valid(base, declaration) && !valid(effective, envelope), "schema roots remain distinct");
    for (const auto& path : {"/metadata/options", "/metadata/support", "/install/cases", "/launch/argv", "/launch/custom_args/defaults", "/operations"}) {
        auto value = effective;
        auto& record = value.at(Json::json_pointer(path))[0];
        record.erase("id");
        check(!valid(value, declaration), std::string("required stable id ") + path);
        record["id"] = 4;
        check(!valid(value, declaration), std::string("typed stable id ") + path);
    }
    for (const auto& path : {"/launch/ports/0/name", "/metadata/display_name", "/metadata/server_name", "/metadata/binary"}) {
        auto value = effective;
        auto pointer = Json::json_pointer(path);
        auto parent = pointer.parent_pointer();
        value.at(parent).erase(pointer.back());
        check(!valid(value, declaration), std::string("required declaration member ") + path);
    }
    for (const auto& literal : {Json{{"op", "remove"}, {"path", "/operations"}}, Json{{"$delete", true}}, Json(), Json::array({1, false})}) {
        auto value = effective;
        value["metadata"]["options"][0]["default_value"] = literal;
        value["metadata"]["config_extra"]["arbitrary"] = literal;
        value["launch"]["argv"][0]["value"] = {{"literal", literal}};
        check(valid(value, declaration), "arbitrary payload remains opaque");
    }
    auto value = base;
    value["edits"] = Json::array({{{"op", "clear"}, {"path", "/" + std::string(4095, 'x')}}});
    check(valid(value, envelope), "pointer character length boundary");
    value["edits"][0]["path"] = "/" + std::string(4096, 'x');
    check(!valid(value, envelope), "pointer maxLength enforced");
    value = base;
    value["edits"] = Json::array();
    for (int i = 0; i < 1024; ++i) value["edits"].push_back({{"op", "clear"}, {"path", "/hooks"}});
    check(valid(value, envelope), "1024 edits admitted");
    value["edits"].push_back(value["edits"][0]);
    check(!valid(value, envelope), "1025 edits rejected");
    auto limits = DefinitionLimits{};
    limits.work_nodes = 0;
    check(!validate_v2_structure(base, envelope, limits).entries.empty(), "structural work admitted before walking");
    limits = {};
    limits.diagnostics = 1;
    auto errors = validate_v2_structure(Json::object(), declaration, limits);
    check(errors.entries.size() == 1 && errors.truncated, "diagnostic cap and truncation");
    limits.diagnostics = 0;
    errors = validate_v2_structure(Json::object(), declaration, limits);
    check(errors.entries.empty() && errors.truncated, "zero-cap still reports failure");
    check(read("algebra.json").size() == 5, "five worked resolver fixtures retained including three rejections");
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
