#include "backend_definition_v2_internal.h"

#include <cstdio>
#include <fstream>

using namespace lemon::backends;
using Json = nlohmann::json;
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
Json read(const std::string& name) {
    std::ifstream stream(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/" + name);
    return Json::parse(stream);
}
const DefinitionKey base_key{DefinitionNamespace::Builtin,"llamacpp",1};
const DefinitionKey child_key{DefinitionNamespace::Builtin,"fork",1};
Json fork() { return {{"schema_version",2},{"id","fork"},{"recipe","fork"},{"extends",{{"builtin","llamacpp"},{"contract",1}}}}; }
DefinitionResolutionResult resolve(const Json& base, const Json& child, const DefinitionLimits& limits = {}) {
    const auto a = base.dump(), b = child.dump();
    const auto snapshot = make_v2_source_snapshot({{child_key,b,"/private/token-secret.json"},{base_key,a,"/home/private.json"}});
    DefinitionResolver resolver(snapshot.snapshot,limits);
    return resolver.resolve(child_key);
}
}
int main() {
    const auto base = read("llamacpp.example.json");
    for (const auto& fixture : read("diagnostics.json")) {
        auto child = fork(); child["edits"] = Json::array({fixture["edit"]});
        const auto result = resolve(base,child);
        check(!result.definition && result.diagnostics.entries.size() == 1,"diagnostic failure has no result");
        if (result.diagnostics.entries.empty()) continue;
        const auto& diagnostic = result.diagnostics.entries[0];
        check(diagnostic.code == fixture["code"] && diagnostic.pointer == fixture["pointer"] && diagnostic.definition == "fork","exact source diagnostic");
        if (fixture.contains("effective_pointer")) check(diagnostic.effective_pointer == fixture["effective_pointer"].get<std::string>(),"exact effective diagnostic");
        check(diagnostic.message.find("private") == std::string::npos && diagnostic.message.find("secret") == std::string::npos,"paths and raw values redacted");
    }
    auto child = fork();
    child["edits"] = Json::array({{{"op","replace"},{"path","/metadata/display_name"},{"value","Llama.cpp GPU"}}});
    const auto equal = resolve(base,child).definition;
    check(equal && equal->provenance->origin_at("/metadata/display_name")->pointer == "/edits/0/value","equal replacement newly authored");
    check(equal && equal->provenance->authorship().back().whole_subtree && equal->provenance->authorship().back().kind == DefinitionEditKind::Replace,"equal replacement journal retained");
    const auto unchanged = equal->provenance->origin_at("/operations/1/transport/path");
    check(unchanged && unchanged->document.id == "llamacpp" && unchanged->pointer == "/merge/operations/1/transport/path","inherited data retains parent origin");
    child = fork();
    child["edits"] = Json::array({{{"op","add"},{"path","/operations"},{"before",{{"id","chat.buffered"}}},{"value",base["merge"]["operations"][0]}}});
    child["edits"][0]["value"]["id"] = "new-chat";
    const auto inserted = resolve(base,child).definition;
    check(inserted && inserted->provenance->origin_at("/operations/2/transport/path")->pointer == "/merge/operations/1/transport/path","insertion preserves logical provenance of shifted record");
    child = fork();
    child["merge"]["rationale"] = Json::array({{{"subject","/operations"},{"match",{{"id","completion.buffered"}}},{"at","/transport/path"},{"text","stable"},{"source","fixture"},{"tests",Json::array({"stable"})}}});
    const auto rationale = child["merge"]["rationale"];
    child["edits"] = Json::array({{{"op","remove"},{"path","/operations"},{"match",{{"id","chat.buffered"}}}}});
    check(resolve(base,child).definition != nullptr,"record-aware rationale survives removal before referent");
    child["merge"]["rationale"][0]["subject"] = "/operations/1";
    child["merge"]["rationale"][0].erase("match"); child["merge"]["rationale"][0].erase("at");
    const auto numeric = resolve(base,child);
    check(!numeric.definition && numeric.diagnostics.entries[0].code == "array_index_forbidden","numeric rationale referent rejected even when pointer exists");
    child["merge"]["rationale"] = rationale;
    child["edits"][0]["match"]["id"] = "completion.buffered";
    const auto removed = resolve(base,child);
    check(!removed.definition && removed.diagnostics.entries[0].code == "invalid_reference" && !removed.diagnostics.entries[0].related.empty() && removed.diagnostics.entries[0].related[0].pointer == "/edits/0","removed rationale target relates originating deletion");
    child["edits"] = Json::array({{{"op","replace"},{"path","/operations"},{"value",Json::array({base["merge"]["operations"][0]})}}});
    const auto replaced = resolve(base,child);
    check(!replaced.definition && !replaced.diagnostics.entries[0].related.empty() && replaced.diagnostics.entries[0].related[0].pointer == "/edits/0","whole-array replacement records deletion of rationale referent");
    child = fork();
    const auto record = base["merge"]["launch"]["argv"][2];
    child["edits"] = Json::array({{{"op","remove"},{"path","/launch/argv"},{"match",{{"id","ctx"}}}},{{"op","add"},{"path","/launch/argv"},{"value",record}}});
    const auto readded = resolve(base,child).definition;
    check(readded && readded->provenance->origin_at("/launch/argv/12/id")->pointer == "/edits/1/value/id","removed then re-added record has new authorship");
    check(readded && readded->provenance->removal_at({"/launch/argv",RecordKey{RecordKey::Field::Id,"ctx"},""})->pointer == "/edits/0","tombstone retained after re-add");
    auto invalid_base = base;
    invalid_base["merge"]["metadata"]["display_name"] = 42;
    const auto inherited = resolve(invalid_base,fork());
    check(!inherited.definition && inherited.diagnostics.entries[0].definition == "llamacpp" && inherited.diagnostics.entries[0].pointer == "/merge/metadata/display_name","inherited structural failure points to parent");
    child = fork();
    child["merge"]["metadata"]["options"] = base["merge"]["metadata"]["options"];
    child["merge"]["metadata"]["options"][1]["id"] = "backend";
    const auto duplicate = resolve(base,child);
    check(!duplicate.definition && duplicate.diagnostics.entries[0].code == "duplicate_identity" && !duplicate.diagnostics.entries[0].related.empty(),"relationship failure includes both origins");
    child = fork();
    child["merge"]["metadata"]["options"][0] = {{"id","x"}};
    auto limits = DefinitionLimits{}; limits.diagnostics = 1;
    const auto truncated = resolve(base,child,limits);
    check(!truncated.definition && truncated.diagnostics.entries.size() == 1 && truncated.diagnostics.truncated,"diagnostics cap preserves truncation");
    child = fork();
    child["edits"] = Json::array({{{"op","replace"},{"path","/metadata/display_name"},{"value",42}},{{"op","replace"},{"path","/metadata/server_name"},{"value",false}}});
    const auto ordered = resolve(base,child);
    check(ordered.diagnostics.entries.size() == 2 && ordered.diagnostics.entries[0].pointer == "/edits/0/value" && ordered.diagnostics.entries[1].pointer == "/edits/1/value","deterministic source ordering");
    std::printf("%d/%d checks passed\n",checks-failures,checks);
    return failures ? 1 : 0;
}
