#include "catalog_admission_fixture.h"

#include <type_traits>

using namespace admission_fixture;
int main() {
    static_assert(!std::is_default_constructible_v<AdmittedCatalog>);
    static_assert(!std::is_default_constructible_v<FrozenBuiltinCatalog>);
    const auto admitted = admit({child("child", "root"), root(), legacy()});
    check(admitted.catalog != nullptr, "frozen mixed graph supports forward references");
    if (admitted.catalog) {
        check(admitted.catalog->metrics().authorized_definitions == 2, "every v2 node authorized");
        check(admitted.catalog->v1_definition("legacy") != nullptr && admitted.catalog->v2_definition("child") != nullptr, "mixed versions retained without conversion");
        check(admitted.catalog->v2_definition("child")->definition().key.space == DefinitionNamespace::External, "intake forces external origin");
    }
    reject({root(), root()}, "id_collision", "duplicate v2 ID rejected");
    reject({root(), legacy("root")}, "id_collision", "cross version ID collision rejected");
    auto value = root("other"); value["recipe"] = "root";
    reject({root(), value}, "recipe_collision", "cross node recipe collision rejected");
    value = legacy("other"); value["recipe"] = "root";
    reject({root(), value}, "recipe_collision", "mixed recipe collision rejected");
    for (const auto* id : {"llamacpp", "cloud", "sdcpp", "llamacpp-hrx"}) {
        value = root(id); value["recipe"] = std::string("external-") + id;
        reject({value}, "id_collision", std::string("builtin identity reserved ") + id);
        value = legacy(id); value["recipe"] = std::string("external-") + id;
        reject({value}, "id_collision", std::string("v1 builtin shadow rejected ") + id);
    }
    for (const auto* section : {"host", "no_broadcast", "llamacpp"}) {
        value = root(); value["merge"]["metadata"]["config_section"] = section;
        reject({value}, "section_collision", std::string("reserved section ") + section);
    }
    value = root("other"); value["merge"]["metadata"]["config_section"] = "root";
    reject({root(), value}, "section_collision", "external section collision");
    for (const auto* name : {"ctx_size", "llamacpp_backend", "host"}) {
        value = root(); value["merge"]["metadata"]["options"] = {{{"id", "option"}, {"name", name}, {"value_type", "string"}, {"default_value", ""}}};
        reject({value}, "option_collision", std::string("reserved option ") + name);
    }
    value = root();
    value["merge"]["metadata"]["options"] = {{{"id", "huge"}, {"name", "huge_value"}, {"value_type", "integer"}, {"default_value", 9007199254740992LL}}};
    reject({value}, "invalid_public_projection", "unsafe integer cannot enter public metadata");
    value["merge"]["metadata"]["options"][0]["default_value"] = 9007199254740991LL;
    check(admit({value}).catalog != nullptr, "safe integer public boundary");
    value["merge"]["metadata"]["options"][0]["default_value"] = Json::object();
    reject({value}, "invalid_public_projection", "structured public default rejected");
    value["merge"]["metadata"]["options"][0]["default_value"] = "wrong-type";
    reject({value}, "invalid_public_projection", "public default type checked");
    value["merge"]["metadata"]["options"][0]["default_value"] = 1;
    value["merge"]["metadata"]["options"][0]["unset_values"] = Json::array({Json::object()});
    reject({value}, "invalid_public_projection", "structured public unset value rejected");
    auto first = root();
    first["merge"]["metadata"]["options"] = {{{"id", "one"}, {"name", "shared_option"}, {"value_type", "string"}, {"default_value", ""}}};
    value = root("other"); value["merge"]["metadata"]["options"] = first["merge"]["metadata"]["options"];
    reject({first, value}, "option_collision", "external public option collision");
    first["merge"]["metadata"]["options"] = {
        {{"id", "one"}, {"name", "one_backend"}, {"value_type", "string"}, {"default_value", ""}},
        {{"id", "two"}, {"name", "two_backend"}, {"value_type", "string"}, {"default_value", ""}}};
    reject({first}, "config_key_collision", "persisted suffix collision");
    for (const auto* field : {"install_recipe", "pin_recipe"}) {
        value = root(); value["merge"]["install"][field] = "llamacpp";
        reject({value}, "install_ownership", std::string("v2 leaf ownership ") + field);
        value["merge"]["install"][field] = "root";
        check(admit({value}).catalog != nullptr, std::string("explicit leaf install identity ") + field);
        reject({value, child("child", "root")}, "install_ownership", std::string("inherited alias requires removal ") + field);
    }
    auto alias = legacy(); alias["install"]["install_recipe"] = "v1-installer"; alias["install"]["pin_recipe"] = "v1-pin";
    check(admit({alias}).catalog != nullptr, "valid legacy aliases preserved");
    reject({root("v1-installer"), alias}, "alias_collision", "v1 install alias reserved before v2");
    value = root("unrelated"); value["recipe"] = "v1-pin";
    reject({alias, value}, "alias_collision", "v1 alias reserves v2 recipe independently");
    value = root("v1-pin"); value["recipe"] = "unrelated";
    reject({alias, value}, "alias_collision", "v1 alias reserves v2 ID independently");
    reject({alias, root("v1-pin")}, "alias_collision", "v1 pin alias reserved");
    reject({child("child", "absent")}, "missing_parent", "missing external dependency explicit");
    reject({legacy(), child("child", "legacy")}, "unsupported_parent_version", "existing v1 parent distinguished");
    value = child("child", "unused"); value["extends"] = {{"builtin", "llamacpp"}, {"contract", 1}};
    reject({value}, "unexported_builtin", "production exports remain empty");
    reject({child("a", "b"), child("b", "a")}, "dependency_cycle", "ambiguous cyclic graph rejects");
    BackendCatalog active({});
    const auto before = active.export_schema();
    auto legacy_collision = legacy(); legacy_collision["recipe"] = "llamacpp";
    const auto collision_reply = active.validate(legacy_collision.dump());
    check(collision_reply["diagnostics"][0]["message"] == "Recipe already owned", "v1 ownership diagnostic remains byte compatible");
    const auto candidate = active.validate(root().dump());
    check(candidate["valid"] == false && candidate["stages"]["catalog_admitted"] == true && candidate["stages"]["executable"] == false,
        "v2 candidate is admitted but never executable valid");
    check(active.export_schema() == before && !backend_catalog(), "private candidate cannot publish");
    bool refused = false;
    try { BackendCatalog executable({{"", root().dump(), DefinitionOrigin::External}}); }
    catch (const DefinitionValidationError& error) { refused = error.diagnostics[0].code == "unsupported_execution_version"; }
    check(refused && !backend_catalog(), "startup cannot activate admitted v2");
    value = child("child", "missing");
    auto response = active.validate(value.dump());
    check(response["diagnostics"][0]["code"] == "missing_parent" && response["diagnostics"][0]["pointer"] == "/extends/external", "candidate missing dependency response");
    value = root(); value["merge"]["launch"]["argv"] = {{{"id", "secret"}, {"args", {"PRIVATE_ARGV_VALUE"}}}};
    value["merge"]["launch"]["environment"]["set"] = {{{"name", "SECRET"}, {"value", "PRIVATE_ENV_VALUE"}}};
    value["merge"]["hooks"]["launch"] = {{"id", "kokoro.voice_index"}, {"params", {{"secret", "PRIVATE_PARAM_VALUE"}}}};
    response = active.validate(value.dump());
    const auto public_bytes = response.dump();
    check(public_bytes.find("PRIVATE_") == std::string::npos && !response.contains("authority") && !response.contains("declaration"), "disclosure allowlist excludes authored execution values");
    const CompositionDiagnostic unsafe{"unknown_key", "/PRIVATE_SERVER_PATH", "/merge/PRIVATE_RAW_VALUE", "PRIVATE_EXCEPTION", {}, {}, {}};
    check(public_admission_diagnostic(unsafe) == Json{{"code", "unknown_key"}, {"definition", ""}, {"pointer", "/merge"}, {"message", "Definition admission failed"}}, "unsafe diagnostic identity pointer and exception excluded");
    const auto v2 = active.export_schema("", 2);
    check(v2["catalog_id"] == before["catalog_id"] && v2["catalog_v2_id"] != before["catalog_id"], "separate admission catalog identity");
    check(v2["accepted_definition_versions"] == Json::array({1, 2}) && v2["executable_definition_versions"] == Json::array({1}), "honest input admission versus execution versions");
    check(v2["admission_manifest"]["manifest_version"] == 1 && v2["export_registry_id"] == installed_builtin_exports().digest(), "separate versioned admission manifest");
    for (std::size_t i = 0; i < before["recipes"].size(); ++i)
        check(v2["recipes"][i]["input_schema_version"] == (before["recipes"][i]["definition_id"].is_null() ? Json() : Json(1)), "honest recipe source version " + std::to_string(i));
    check(active.export_schema("", 1) == before && !before.contains("catalog_v2_id") && !installed_definition_features().contains("accepted_definition_versions"), "legacy projection remains isolated");
    return finish();
}
