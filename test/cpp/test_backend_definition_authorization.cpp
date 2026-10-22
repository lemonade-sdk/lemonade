#include "backend_definition_export_fixture.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <type_traits>

using namespace export_fixture;
namespace {
int checks = 0, failures = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
bool has_code(const CompositionDiagnostics& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.entries.begin(), diagnostics.entries.end(), [&](const auto& error) { return error.code == code; });
}
Json child(const std::string& id = "child", const std::string& parent = "") {
    return {{"schema_version", 2}, {"id", id}, {"recipe", id}, {"extends", parent.empty() ?
        Json{{"builtin", "test-export"}, {"contract", 1}} : Json{{"external", parent}}}};
}
Json edit(const std::string& op, const std::string& path, const Json& value, const std::string& at = "") {
    Json result = {{"op", op}, {"path", path}, {"value", value}};
    if (!at.empty()) { result["match"] = {{"id", "chat.buffered"}}; result["at"] = at; }
    return result;
}
DefinitionAuthorizationResult authorize(const std::vector<Json>& children, BuiltinExportContract policy = contract(), DefinitionLimits limits = {}) {
    const auto base = read_bytes("export.synthetic.json");
    std::vector<std::string> bytes;
    for (const auto& value : children) bytes.push_back(value.dump());
    std::vector<DefinitionSourceView> sources = {{{DefinitionNamespace::Builtin, "test-export", 1}, base, "/PRIVATE_PATH"}};
    for (std::size_t i = 0; i < children.size(); ++i) sources.push_back({{DefinitionNamespace::External, children[i].at("id"), 0}, bytes[i], "/PRIVATE_PATH"});
    const auto registry = BuiltinCatalogLoader::test_exports({policy}, snapshot().snapshot, installed_hook_abis(), v2::export_registry_digest({policy}, installed_hook_abis()));
    const auto frozen = BuiltinCatalogLoader::test_freeze(sources, registry, limits);
    if (!frozen.snapshot) return {nullptr, frozen.diagnostics};
    return DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, children.back().at("id"), 0});
}
void reject(const std::string& label, const Json& value, const std::string& code) {
    const auto result = authorize({value});
    check(!result.definition && has_code(result.diagnostics, code), label);
    if (!has_code(result.diagnostics, code)) for (const auto& error : result.diagnostics.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
}
std::string graph_error(const std::function<void(ResolvedDefinition&, BuiltinExportContract&, std::vector<LogicalHookSlot>&)>& change, bool reauthor = false) {
    DefinitionResolver resolver(snapshot().snapshot);
    const auto base = resolver.resolve({DefinitionNamespace::Builtin, "test-export", 1}).definition;
    auto value = *base;
    value.key = {DefinitionNamespace::External, "test", 0};
    auto policy = contract();
    std::vector<LogicalHookSlot> inherited;
    for (const auto& slot : policy.slots) inherited.push_back(slot.slot);
    change(value, policy, inherited);
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    auto document = std::make_shared<const DefinitionKey>(base->key);
    const auto root = v2::authored(base->declaration, std::make_shared<v2::Origin>(v2::Origin{document, {}, {}}), budget);
    try {
        const auto current = reauthor ? v2::authored(base->declaration, std::make_shared<v2::Origin>(v2::Origin{
            std::make_shared<const DefinitionKey>(value.key), {}, {}}), budget) : root;
        v2::verify_surviving_grants(value, *base, *base, &policy, inherited, root, current, budget);
    } catch (const v2::AuthorizationFailure& failure) { return failure.code; }
    return "";
}
}

int main() {
    static_assert(!std::is_default_constructible_v<FrozenDefinitionSnapshot>);
    static_assert(!std::is_default_constructible_v<ResolvedAuthority>);
    static_assert(!std::is_default_constructible_v<AuthorizedDefinition>);
    static_assert(!std::is_constructible_v<DefinitionAuthorizer, std::shared_ptr<const DefinitionSourceSnapshot>>);
    static_assert(!std::is_constructible_v<AuthorizedDefinition, ResolvedDefinition>);
    static_assert(!std::is_constructible_v<Json, ResolvedAuthority>);
    static_assert(!std::is_constructible_v<Json, AuthorizedDefinition>);
    const auto base = read_bytes("export.synthetic.json");
    const auto promoted = load_v2_authority_snapshot({{{DefinitionNamespace::Builtin, "test-export", 1}, base, ""}});
    check(!promoted.snapshot && has_code(promoted.diagnostics, "origin_promotion"), "caller builtin namespace cannot mint trust");
    const auto inherited = authorize({child()});
    check(inherited.definition != nullptr, "inherit original logical slots");
    if (!inherited.definition) for (const auto& error : inherited.diagnostics.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
    if (inherited.definition) {
        check(inherited.definition->definition().key.space == DefinitionNamespace::External, "external origin remains external");
        check(inherited.definition->definition().definition_hash.size() == 71, "authorization preserves resolver identity");
    }
    auto value = child();
    value["merge"]["metadata"]["display_name"] = "Allowed descendant";
    check(authorize({value}).definition != nullptr, "typed mutable data allowed");
    value["merge"]["metadata"]["display_name"] = std::string(129, 'x');
    reject("mutable length enforced", value, "invalid_mutable_input");
    value = child();
    value["edits"] = Json::array({edit("replace", "/hooks/launch/id", "kokoro.voice_index")});
    reject("identical id reauthoring", value, "authored_hook_selection");
    value["edits"] = Json::array({edit("replace", "/hooks/launch", Json{{"id", "kokoro.voice_index"}, {"params", Json::object()}})});
    reject("identical object reauthoring", value, "authored_hook_selection");
    value["edits"] = Json::array({edit("replace", "/hooks/launch/params", Json::object())});
    reject("identical params reauthoring", value, "authored_hook_selection");
    value["edits"] = Json::array({edit("replace", "/hooks/launch/id", "PRIVATE_HOOK"), edit("replace", "/hooks/launch/id", "kokoro.voice_index")});
    reject("hidden overwritten hook selection", value, "authored_hook_selection");
    const auto hidden = authorize({value});
    check(!hidden.diagnostics.entries.empty() && hidden.diagnostics.entries[0].pointer == "/edits/0/value", "hidden selection diagnosed at authored write");
    const auto rendered = hidden.diagnostics.entries.empty() ? "" : hidden.diagnostics.entries[0].message;
    check(rendered.find("PRIVATE_HOOK") == std::string::npos && rendered.find("PRIVATE_PATH") == std::string::npos, "diagnostics redact authored values and labels");
    value = child();
    value["merge"]["hooks"]["launch"] = {{"id", "kokoro.voice_index"}};
    reject("merge authored hook selection", value, "authored_hook_selection");
    value = child();
    value["edits"] = Json::array({edit("replace", "/operations", Json::parse(base)["merge"]["operations"])});
    reject("subtree replacement reauthors retained hooks", value, "authored_hook_selection");
    value = child();
    value["edits"] = Json::array({edit("clear", "/operations", nullptr, "/request/hooks")});
    value["edits"][0].erase("value");
    reject("mandatory sanitizers retained with owner", value, "mandatory_slot_removed");
    value = child();
    value["edits"] = Json::array({edit("replace", "/launch/program/source", "external_or_managed")});
    reject("sealed hook input", value, "sealed_input_changed");
    value = child();
    value["edits"] = Json::array({edit("replace", "/launch/readiness/timeout", 1)});
    reject("unlisted consumed data is sealed", value, "sealed_data_changed");
    value = child();
    value["edits"] = Json::array({edit("add", "/metadata/compatibility", "TrustedCompatibility"), Json{{"op", "remove"}, {"path", "/metadata/compatibility"}}});
    reject("hidden compatibility attachment", value, "forbidden_authority_control");
    value["edits"][0]["path"] = "/metadata/origin";
    value["edits"][1]["path"] = "/metadata/origin";
    reject("hidden serialized origin promotion", value, "forbidden_authority_control");
    auto plain = Json::parse(base);
    plain["id"] = plain["recipe"] = "plain";
    plain["merge"].erase("hooks");
    plain["merge"]["artifacts"] = Json::object();
    plain["merge"]["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
    for (auto& operation : plain["merge"]["operations"]) { operation["request"].erase("hooks"); operation.erase("response"); }
    auto plain_bytes = plain.dump();
    const auto no_hooks = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, plain_bytes, ""}});
    check(no_hooks.snapshot && DefinitionAuthorizer(no_hooks.snapshot).authorize({DefinitionNamespace::External, "plain", 0}).definition, "unprivileged declaration needs no export");
    plain["edits"] = Json::array({edit("add", "/hooks", Json{{"launch", {{"id", "kokoro.voice_index"}}}}), Json{{"op", "remove"}, {"path", "/hooks"}}});
    plain_bytes = plain.dump();
    const auto overwritten = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, plain_bytes, ""}});
    check(overwritten.snapshot && has_code(DefinitionAuthorizer(overwritten.snapshot).authorize({DefinitionNamespace::External, "plain", 0}).diagnostics, "authored_hook_selection"), "hidden hook in hookless graph rejected");
    auto hidden_merge = plain;
    hidden_merge["merge"]["hooks"] = {{"launch", {{"id", "PRIVATE_HOOK"}}}};
    hidden_merge["edits"] = Json::array({Json{{"op", "remove"}, {"path", "/hooks"}}});
    auto hidden_merge_bytes = hidden_merge.dump();
    const auto merged = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, hidden_merge_bytes, ""}});
    check(merged.snapshot && has_code(DefinitionAuthorizer(merged.snapshot).authorize({DefinitionNamespace::External, "plain", 0}).diagnostics,
        "authored_hook_selection"), "hidden initial merge selection rejected");
    const std::vector<std::string> stages = {"install", "artifacts", "launch", "start_failure", "availability", "model_discovery", "model_download",
        "remote_context", "backend", "install_context", "runtime_options", "environment", "system", "unknown_stage"};
    for (const auto& stage : stages) {
        auto selected = hidden_merge;
        selected["merge"]["hooks"] = {{stage, {{"id", "PRIVATE_HOOK"}}}};
        auto bytes = selected.dump();
        const auto frozen = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, bytes, ""}});
        const auto result = DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, "plain", 0});
        check(frozen.snapshot && !result.definition && has_code(result.diagnostics, "authored_hook_selection"), "inspect root stage " + stage);
    }
    for (const auto& position : {"install", "artifact", "handler", "request", "response"}) {
        auto selected = plain;
        selected.erase("edits");
        Json removal = {{"op", "remove"}};
        const Json hook = {{"id", "PRIVATE_HOOK"}};
        if (std::string(position) == "install") {
            selected["merge"]["install"]["hook"] = hook;
            removal["path"] = "/install/hook";
        } else if (std::string(position) == "artifact") {
            selected["merge"]["artifacts"]["main"] = {{"role", "main"}, {"strategy", "hook"}, {"hook", hook}};
            removal["path"] = "/artifacts/main";
        } else {
            removal["path"] = "/operations";
            removal["match"] = {{"id", "chat.buffered"}};
            const auto field = std::string(position) == "handler" ? "/handler" :
                std::string(position) == "request" ? "/request/hooks" : "/response";
            removal["at"] = field;
            if (std::string(position) == "handler") selected["merge"]["operations"][0]["handler"] = hook;
            else selected["merge"]["operations"][0][position]["hooks"] = Json::array({hook});
        }
        selected["edits"] = Json::array({removal});
        auto bytes = selected.dump();
        const auto frozen = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, bytes, ""}});
        const auto result = DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, "plain", 0});
        check(frozen.snapshot && !result.definition && has_code(result.diagnostics, "authored_hook_selection"), "inspect hidden " + std::string(position));
    }
    auto optional = contract();
    for (auto& slot : optional.slots) if (slot.slot.stage == "launch") slot.removal = SlotRemoval::Independent;
    repin(optional);
    auto diminished = child("diminished");
    diminished["edits"] = Json::array({Json{{"op", "remove"}, {"path", "/hooks/launch"}},
        edit("replace", "/launch/prepare_order", Json::array({"backend", "artifacts", "program", "ports"}))});
    auto grandchild = child("grandchild", "diminished");
    check(authorize({diminished, grandchild}, optional).definition != nullptr, "external chain equal or diminished");
    grandchild["edits"] = Json::array({edit("add", "/hooks/launch", Json{{"id", "kokoro.voice_index"}, {"params", Json::object()}}),
        edit("replace", "/launch/prepare_order", Json::parse(base)["merge"]["launch"]["prepare_order"])});
    check(has_code(authorize({diminished, grandchild}, optional).diagnostics, "authored_hook_selection"), "removed grant cannot be restored in descendant");
    auto removable = contract();
    for (auto& slot : removable.slots) if (slot.slot.owner_kind == SlotOwnerKind::Operation) slot.removal = SlotRemoval::WithOwner;
    repin(removable);
    value = child();
    value["edits"] = Json::array({Json{{"op", "remove"}, {"path", "/operations"}, {"match", {{"id", "chat.buffered"}}}}});
    auto replacement = Json::parse(base)["merge"]["operations"][0];
    replacement["id"] = "laundered";
    replacement["request"].erase("hooks"); replacement.erase("response");
    auto allowed = replacement; allowed["operation"] = "completion"; allowed["transport"]["path"] = "/completion";
    auto removal = value;
    removal["edits"].push_back(edit("add", "/operations", allowed));
    check(authorize({removal}, removable).definition != nullptr, "explicitly removable owner and grants");
    value["edits"].push_back(edit("add", "/operations", replacement));
    check(has_code(authorize({value}, removable).diagnostics, "protected_owner_recreated"), "owner removal cannot launder protected behavior");
    value = child();
    auto unrelated = replacement;
    unrelated["id"] = "unrelated"; unrelated["operation"] = "completion"; unrelated["transport"]["path"] = "/completion";
    auto add = edit("add", "/operations", unrelated); add["before"] = {{"id", "chat.buffered"}};
    value["edits"] = Json::array({add});
    check(authorize({value}).definition != nullptr, "unrelated operation insertion preserves logical slot");
    value = child();
    value["edits"] = Json::array({edit("add", "/operations", Json{{"/payload", {{"literal", {{"hooks", Json::array({{{"id", "ordinary"}}})}}}}}}, "/request/set")});
    reject("literal data remains data but sealed unless mutable", value, "sealed_data_changed");
    check(graph_error([](auto& d, auto&, auto&) { d.declaration["hooks"]["launch"]["params"]["x"] = 1; }) == "hook_parameters_changed", "final parameter digest");
    check(graph_error([](auto& d, auto&, auto&) { std::reverse(d.declaration["operations"][0]["request"]["hooks"].begin(), d.declaration["operations"][0]["request"]["hooks"].end()); }) == "hook_order_changed", "final hook ordering");
    {
        DefinitionLimits limits;
        std::size_t retained = 0;
        v2::Budget budget{limits, {}, retained};
        DefinitionResolver resolver(snapshot().snapshot);
        auto selections = v2::hook_selections(resolver.resolve({DefinitionNamespace::Builtin, "test-export", 1}).definition->declaration, budget);
        const auto policy = contract();
        auto selection = *std::find_if(selections.begin(), selections.end(), [](const auto& item) { return item.slot.owner_kind == SlotOwnerKind::Operation; });
        const auto slot = std::find_if(policy.slots.begin(), policy.slots.end(), [&](const auto& item) { return v2::slot_json(item.slot) == v2::slot_json(selection.slot); });
        selection.owner_semantics["delivery"] = "streaming";
        std::string code;
        try { v2::verify_slot_shape(selection, *slot, {{DefinitionNamespace::External, "test", 0}, ""}, budget); }
        catch (const v2::AuthorizationFailure& failure) { code = failure.code; }
        check(code == "hook_owner_changed", "final owner semantics");
    }
    check(graph_error([](auto&, auto& c, auto&) { c.slots[0].hook_abi_revision = 2; }) == "hook_abi_changed", "final hook ABI");
    check(graph_error([](auto& d, auto&, auto&) { d.declaration["hooks"]["environment"] = d.declaration["hooks"]["launch"]; d.declaration["hooks"].erase("launch"); }) == "grant_growth", "final stage relocation");
    check(graph_error([](auto& d, auto&, auto&) {
        d.declaration["operations"][0]["response"]["hooks"] = d.declaration["operations"][0]["request"]["hooks"];
        d.declaration["operations"][0]["request"]["hooks"] = Json::array();
    }) == "grant_growth", "final operation stage relocation");
    check(graph_error([](auto& d, auto&, auto&) { d.declaration["operations"][0]["id"] = "relocated"; }) == "grant_growth", "final operation relocation");
    check(graph_error([](auto&, auto&, auto& slots) { slots.pop_back(); }) == "grant_growth", "final immediate parent subset");
    check(graph_error([](auto& d, auto&, auto&) { d.declaration["operations"][0]["request"]["hooks"] = Json::array(); }) == "mandatory_slot_removed", "final mandatory slot obligations");
    check(graph_error([](auto&, auto&, auto&) {}, true) == "hook_provenance_changed", "final identical reauthoring provenance");
    value = child();
    value["edits"] = Json::array({edit("replace", "/operations", "streaming", "/delivery"),
        edit("replace", "/operations", "buffered", "/delivery")});
    reject("hidden owner change", value, "authored_owner_change");
    auto missing = child(); missing["extends"]["builtin"] = "llamacpp";
    auto missing_bytes = missing.dump();
    const auto stock = load_v2_authority_snapshot({{{DefinitionNamespace::External, "child", 0}, missing_bytes, ""}});
    check(stock.snapshot && has_code(DefinitionAuthorizer(stock.snapshot).authorize({DefinitionNamespace::External, "child", 0}).diagnostics,
        "unexported_builtin"), "stock llamacpp export remains unavailable");
    value = child();
    value["edits"] = Json::array({edit("merge", "/operations", Json{{"delivery", "streaming"}}, "/" )});
    value["edits"][0].erase("at");
    value["edits"].push_back(edit("replace", "/operations", "buffered", "/delivery"));
    reject("hidden merged owner change", value, "authored_owner_change");
    value = child();
    value["edits"] = Json::array({edit("replace", "/artifacts/main/role", "draft"), edit("replace", "/artifacts/main/role", "main")});
    reject("hidden artifact role change", value, "authored_owner_change");
    auto dependent = optional;
    dependent.slots[1].required_slots.push_back(dependent.slots[0].slot);
    repin(dependent);
    check(has_code(authorize({diminished}, dependent).diagnostics, "required_slot_removed"), "retained slot dependencies");
    dependent = optional;
    dependent.slots[1].required_phases.push_back("launch");
    repin(dependent);
    check(has_code(authorize({diminished}, dependent).diagnostics, "required_phase_removed"), "retained phase dependencies");
    auto all_optional = contract();
    for (auto& slot : all_optional.slots) if (slot.slot.stage == "request") slot.removal = SlotRemoval::Independent;
    repin(all_optional);
    auto cleared = child();
    cleared["edits"] = Json::array({Json{{"op", "clear"}, {"path", "/operations"}, {"match", {{"id", "chat.buffered"}}}, {"at", "/request/hooks"}}});
    check(authorize({cleared}, all_optional).definition != nullptr, "whole removable hook list can diminish");
    cleared["edits"] = Json::array({edit("replace", "/operations", Json::array({Json::parse(base)["merge"]["operations"][0]["request"]["hooks"][0]}), "/request/hooks")});
    check(has_code(authorize({cleared}, all_optional).diagnostics, "authored_hook_selection"), "partial hook list replacement reauthors survivors");
    auto plain_literal = plain;
    plain_literal.erase("edits");
    plain_literal["merge"]["operations"][0]["request"]["set"]["/payload"] = {{"literal", {{"hooks", Json::array({{{"id", "ordinary"}}})}, {"authority", "ordinary"}}}};
    auto literal_bytes = plain_literal.dump();
    const auto literal = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, literal_bytes, ""}});
    check(literal.snapshot && DefinitionAuthorizer(literal.snapshot).authorize({DefinitionNamespace::External, "plain", 0}).definition, "hook-shaped literal is ordinary data");
    literal_bytes.assign("corrupted after freeze");
    check(DefinitionAuthorizer(literal.snapshot).authorize({DefinitionNamespace::External, "plain", 0}).definition != nullptr, "loader snapshot owns frozen source bytes");
    check(has_code(DefinitionAuthorizer(literal.snapshot).authorize({DefinitionNamespace::Builtin, "plain", 1}).diagnostics,
        "origin_promotion"), "authorizer rejects builtin leaf promotion");
    auto plain_source = plain_literal.dump();
    const auto pure_snapshot = make_v2_source_snapshot({{{DefinitionNamespace::External, "plain", 0}, plain_source, ""}});
    DefinitionResolver pure(pure_snapshot.snapshot);
    const auto pure_result = pure.resolve({DefinitionNamespace::External, "plain", 0});
    DefinitionLimits shared;
    shared.work_nodes = pure_result.definition->metrics.work_nodes;
    const auto bounded = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, plain_source, ""}}, shared);
    const auto exhausted = DefinitionAuthorizer(bounded.snapshot).authorize({DefinitionNamespace::External, "plain", 0});
    check(bounded.snapshot && !exhausted.definition && has_code(exhausted.diagnostics, "limit_exceeded"), "inspection shares resolver work budget");
    check(graph_error([](auto& d, auto& c, auto&) { d.declaration["hooks"].erase("launch"); c.slots[0].removal = SlotRemoval::Independent; d.declaration["artifacts"]["other"] = d.declaration["artifacts"]["main"]; d.declaration["artifacts"].erase("main"); }) == "grant_growth", "final artifact relocation");
    DefinitionLimits no_diagnostics;
    no_diagnostics.diagnostics = 0;
    const auto silent_snapshot = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, hidden_merge_bytes, ""}}, no_diagnostics);
    const auto silent_result = DefinitionAuthorizer(silent_snapshot.snapshot).authorize({DefinitionNamespace::External, "plain", 0});
    check(silent_snapshot.snapshot && !silent_result.definition && silent_result.diagnostics.truncated && silent_result.diagnostics.entries.empty(), "diagnostic bound remains fail closed");
    DefinitionLimits retention;
    retention.retained_logical_bytes = pure_result.definition->metrics.retained_logical_bytes;
    const auto retained_snapshot = load_v2_authority_snapshot({{{DefinitionNamespace::External, "plain", 0}, plain_source, ""}}, retention);
    const auto retained_result = DefinitionAuthorizer(retained_snapshot.snapshot).authorize({DefinitionNamespace::External, "plain", 0});
    check(retained_snapshot.snapshot && !retained_result.definition && has_code(retained_result.diagnostics, "limit_exceeded"), "inspection shares retention budget");
    check(graph_error([](auto& d, auto& c, auto&) {
        auto& hooks = d.declaration["operations"][0]["request"]["hooks"];
        hooks.push_back(hooks[0]);
        for (auto& policy : c.slots) if (policy.slot.stage == "request")
            policy.ordered_hooks.push_back(hooks[0]["id"].template get<std::string>());
    }) == "ambiguous_authority", "final duplicate logical slots");
    value = child();
    value["edits"] = Json::array({edit("add", "/operations", replacement)});
    check(has_code(authorize({value}).diagnostics, "protected_owner_recreated"), "retained owner cannot acquire hookless alias");
    auto endpoint_alias = replacement;
    endpoint_alias["operation"] = "completion";
    value["edits"] = Json::array({edit("add", "/operations", endpoint_alias)});
    check(has_code(authorize({value}).diagnostics, "protected_owner_recreated"), "different operation kind cannot alias protected endpoint");
    auto semantic_alias = replacement;
    semantic_alias["transport"]["path"] = "/another-path";
    value["edits"] = Json::array({edit("add", "/operations", semantic_alias)});
    check(has_code(authorize({value}).diagnostics, "protected_owner_recreated"), "different endpoint cannot alias protected operation kind");
    DefinitionLimits tight;
    tight.work_nodes = 1;
    check(!authorize({child()}, contract(), tight).definition, "authorization respects work bound");
    check(has_code(DefinitionAuthorizer(nullptr).authorize({DefinitionNamespace::External, "child", 0}).diagnostics, "invalid_authority_snapshot"), "null authority snapshot fails closed");
    int compiled = 0, bound = 0, validated = 0, published = 0;
    value = child(); value["merge"]["hooks"]["launch"] = {{"id", "kokoro.voice_index"}};
    const auto rejected = authorize({value});
    const auto execute = [&](const AuthorizedDefinition&) { ++compiled; ++bound; ++validated; ++published; };
    if (rejected.definition) execute(*rejected.definition);
    check(compiled == 0 && bound == 0 && validated == 0 && published == 0, "rejection precedes executable consumers");
    if (inherited.definition) execute(*inherited.definition);
    check(compiled == 1 && bound == 1 && validated == 1 && published == 1, "authorized token gates test consumers");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
