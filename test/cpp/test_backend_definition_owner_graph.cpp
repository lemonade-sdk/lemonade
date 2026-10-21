#include "backend_definition_export_fixture.h"

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace export_fixture;
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}
bool has_code(const CompositionDiagnostics& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.entries.begin(), diagnostics.entries.end(), [&](const auto& error) { return error.code == code; });
}
BuiltinExportContract siblings() { return contract("export.siblings.synthetic", "test-siblings"); }
Json child(const std::string& id = "child", const std::string& parent = "") {
    return {{"schema_version", 2}, {"id", id}, {"recipe", id}, {"extends", parent.empty() ?
        Json{{"builtin", "test-siblings"}, {"contract", 1}} : Json{{"external", parent}}}};
}
Json edit(const std::string& op, const std::string& id, const std::string& at = "") {
    Json result = {{"op", op}, {"path", "/operations"}, {"match", {{"id", id}}}};
    if (!at.empty()) result["at"] = at;
    return result;
}
Json addition(Json operation) { return {{"op", "add"}, {"path", "/operations"}, {"value", std::move(operation)}}; }
Json source() { return Json::parse(read_bytes("export.siblings.synthetic.json")); }
Json alias(std::size_t index = 0, bool hooks = false) {
    auto value = source()["merge"]["operations"][index];
    value["id"] = "new-owner";
    if (!hooks) { value["request"].erase("hooks"); value.erase("response"); }
    return value;
}
DefinitionAuthorizationResult authorize(const std::vector<Json>& children, BuiltinExportContract policy = siblings(),
    std::string base = read_bytes("export.siblings.synthetic.json"), DefinitionLimits limits = {}) {
    const auto input = make_v2_source_snapshot({{{DefinitionNamespace::Builtin, "test-siblings", 1}, base, ""}});
    const auto registry = BuiltinCatalogLoader::test_exports({policy}, input.snapshot, installed_hook_abis(), v2::export_registry_digest({policy}, installed_hook_abis()));
    std::vector<std::string> bytes;
    for (const auto& value : children) bytes.push_back(value.dump());
    std::vector<DefinitionSourceView> sources = {{{DefinitionNamespace::Builtin, "test-siblings", 1}, base, ""}};
    for (std::size_t i = 0; i < children.size(); ++i) sources.push_back({{DefinitionNamespace::External, children[i].at("id"), 0}, bytes[i], ""});
    const auto frozen = BuiltinCatalogLoader::test_freeze(sources, registry, limits);
    if (!frozen.snapshot) return {nullptr, frozen.diagnostics};
    return DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, children.back().at("id"), 0});
}
void reject(const std::string& label, const std::vector<Json>& children, const std::string& code, BuiltinExportContract policy = siblings()) {
    const auto result = authorize(children, std::move(policy));
    check(!result.definition && has_code(result.diagnostics, code), label);
    if (!has_code(result.diagnostics, code)) for (const auto& error : result.diagnostics.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
}
void audit_reject(const std::string& label, const std::string& code,
    const std::function<void(BuiltinExportContract&)>& change, bool pin = true) {
    auto policy = siblings();
    change(policy);
    if (pin) repin(policy);
    const auto result = v2::audit_export_contracts({policy}, snapshot("export.siblings.synthetic", "test-siblings").snapshot,
        installed_hook_abis(), v2::export_registry_digest({policy}, installed_hook_abis()));
    check(has_code(result, code), label);
}
std::string raw_graph_error(const std::function<void(ResolvedDefinition&, ResolvedDefinition&, BuiltinExportContract&)>& change,
    bool reauthor = false, bool retain_parent_provenance = false) {
    auto base = *DefinitionResolver(snapshot("export.siblings.synthetic", "test-siblings").snapshot).resolve({DefinitionNamespace::Builtin, "test-siblings", 1}).definition;
    auto policy = siblings();
    policy.slots.clear();
    for (auto& operation : base.declaration["operations"]) { operation["request"].erase("hooks"); operation.erase("response"); }
    for (auto& owner : policy.owners) { owner.request_hooks.clear(); owner.response_hooks.clear(); }
    auto parent = base, current = base;
    current.key = {DefinitionNamespace::External, "child", 0};
    change(current, parent, policy);
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    const auto origin = std::make_shared<v2::Origin>(v2::Origin{std::make_shared<const DefinitionKey>(base.key), {}, {}});
    const auto before = v2::authored(retain_parent_provenance ? base.declaration : parent.declaration, origin, budget);
    const auto after = reauthor ? v2::authored(current.declaration,
        std::make_shared<v2::Origin>(v2::Origin{std::make_shared<const DefinitionKey>(current.key), {}, {}}), budget) : before;
    try {
        v2::verify_surviving_grants(current, parent, base, &policy, {}, before, after, budget);
    } catch (const v2::AuthorizationFailure& failure) { return failure.code; }
    return "";
}
std::string final_chain_error() {
    const auto base = DefinitionResolver(snapshot("export.siblings.synthetic", "test-siblings").snapshot).resolve({DefinitionNamespace::Builtin, "test-siblings", 1}).definition;
    auto policy = siblings();
    policy.owners[0].request_hooks.clear();
    std::vector<LogicalHookSlot> inherited;
    for (const auto& slot : policy.slots) inherited.push_back(slot.slot);
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    const auto root = v2::authored(base->declaration, std::make_shared<v2::Origin>(v2::Origin{std::make_shared<const DefinitionKey>(base->key), {}, {}}), budget);
    try { v2::verify_surviving_grants(*base, *base, *base, &policy, inherited, root, root, budget); }
    catch (const v2::AuthorizationFailure& failure) { return failure.code; }
    return "";
}
void pin_source(BuiltinExportContract& policy, const std::string& base) {
    const auto input = make_v2_source_snapshot({{{DefinitionNamespace::Builtin, "test-siblings", 1}, base, ""}});
    const auto resolved = DefinitionResolver(input.snapshot).resolve({DefinitionNamespace::Builtin, "test-siblings", 1}).definition;
    policy.expected_source_digest = resolved->source_digest;
    policy.expected_definition_hash = resolved->definition_hash;
    policy.expected_dependency_digest = resolved->dependency_digest;
    repin(policy);
}
void check_endpoints() {
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    auto left = alias(), right = alias(1);
    right["operation"] = "embeddings";
    right["transport"]["path"] = left["transport"]["path"];
    right["transport"]["kind"] = "http_sse";
    right["delivery"] = "streaming";
    check(v2::operation_owners_collide(left, right, budget), "EndpointIdentityIgnoresEnvelopeAndDelivery");
    right["transport"]["path"] = "/unrelated";
    check(!v2::operation_owners_collide(left, right, budget), "DeterministicUnrelatedEndpoint");
    right["transport"]["path"] = left["transport"]["path"];
    right["transport"]["method"] = "GET";
    check(!v2::operation_owners_collide(left, right, budget), "DistinctEndpointMethod");
    right["transport"]["method"] = "POST";
    right["transport"]["port"] = "other";
    check(!v2::operation_owners_collide(left, right, budget), "DistinctEndpointPort");
    right["transport"]["port"] = "http";
    right["transport"]["path"] = "/unrelated";
    right["operation"] = left["operation"];
    check(v2::operation_owners_collide(left, right, budget), "PublicOperationKindConservative");
    right["operation"] = "embeddings";
    for (const auto& path : {"relative", "/a/../chat", "/%63hat", "/a//b", "/chat?x=1", "/chat#fragment", "/{dynamic}", "/a\\b"}) {
        right["transport"]["path"] = path;
        check(v2::operation_owners_collide(left, right, budget), "UncertainEndpointHasNoExemption:" + std::string(path));
    }
    right["transport"]["path"] = "/unrelated";
    right["transport"]["method"] = "post";
    check(v2::operation_owners_collide(left, right, budget), "UncertainMethodHasNoExemption");
    right["transport"]["method"] = "POST";
    right["transport"]["kind"] = "realtime";
    check(v2::operation_owners_collide(left, right, budget), "UncertainTransportHasNoExemption");
}

}

int main(int argc, char** argv) {
    check_endpoints();
    if (argc == 2 && std::string(argv[1]) == "--endpoints") {
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    const auto policy = siblings();
    const auto pins = Json::parse(read_bytes("export.siblings.synthetic.pins.json"));
    const auto input = snapshot("export.siblings.synthetic", "test-siblings");
    const auto resolved = DefinitionResolver(input.snapshot).resolve({DefinitionNamespace::Builtin, "test-siblings", 1}).definition;
    check(resolved && resolved->source_digest == policy.expected_source_digest && resolved->definition_hash == policy.expected_definition_hash &&
        resolved->dependency_digest == policy.expected_dependency_digest, "SiblingSourcePins");
    check(v2::owner_graph_digest(policy.owners) == policy.owner_graph_digest && v2::export_policy_digest(policy) == policy.policy_digest &&
        v2::export_registry_digest({policy}, installed_hook_abis()) == pins.at("registry"), "SiblingPolicyPins");
    const auto admitted = authorize({child()});
    check(bool(admitted.definition), "InheritedSameKindSiblingAdmitted");
    check(admitted.definition && admitted.definition->definition().declaration.at("operations").size() == 11, "InheritedEndpointSiblingAdmitted");
    check(admitted.definition && admitted.definition->definition().declaration["operations"][4]["request"]["hooks"].empty(), "InheritedHookFreeCompletionAdmitted");

    const std::vector<std::string> ids = {"chat.buffered", "completion.buffered", "responses.buffered", "chat.streaming", "completion.streaming",
        "responses.streaming", "embeddings.buffered", "reranking.buffered", "slots.buffered", "slot_action.buffered", "tokenize.buffered"};
    const std::vector<unsigned> kinds = {0, 1, 2, 0, 1, 2, 3, 4, 15, 16, 17};
    const std::vector<std::string> names = {"chat_completion", "completion", "responses", "chat_completion", "completion", "responses",
        "embeddings", "reranking", "slots", "slot_action", "tokenize"};
    const std::vector<std::string> paths = {"/v1/chat/completions", "/v1/completions", "/v1/responses", "/v1/chat/completions", "/v1/completions",
        "/v1/responses", "/v1/embeddings", "/v1/rerank", "/slots", "/slots/{slot_id}?action={action}", "/tokenize"};
    const std::vector<std::vector<std::string>> requests = {{"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"},
        {"llamacpp.max_tokens_alias"}, {"llamacpp.tool_schema_limits"}, {"llamacpp.max_tokens_alias", "llamacpp.tool_schema_limits"}, {},
        {"llamacpp.tool_schema_limits"}, {}, {}, {}, {}, {}};
    const std::vector<std::vector<std::string>> responses = {{"llamacpp.client_model"}, {"llamacpp.client_model"}, {"llamacpp.client_model"}, {}, {}, {}, {}, {}, {}, {}, {}};
    check(policy.owners.size() == ids.size(), "AllElevenOwnersExplicit");
    DefinitionLimits limits;
    std::size_t retained = 0;
    v2::Budget budget{limits, {}, retained};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& owner = policy.owners[i];
        check(owner.id == ids[i] && owner.request_hooks == requests[i] && owner.response_hooks == responses[i] && owner.handler_hooks.empty(), "ExactIndependentChains:" + ids[i]);
        check(owner.semantics.at("model_modes") == Json::array({"*"}) && owner.sealed_fields == v2::operation_owner_fields(), "CompleteOwnerFields:" + ids[i]);
        const bool streaming = i >= 3 && i <= 5;
        check(owner.semantics.at("operation") == names[i] && owner.semantics.at("delivery") == (streaming ? "streaming" : "buffered") &&
            owner.semantics.at("transport") == Json({{"kind", streaming ? "http_sse" : "http_json"}, {"method", i == 8 ? "GET" : "POST"},
                {"path", paths[i]}, {"port", "http"}, {"timeout", 1}}) && !owner.semantics.contains("formats") && !owner.semantics.contains("format_metadata"),
            "IndependentOwnerSemantics:" + ids[i]);
        for (const auto* view : {"linux_x64", "linux_arm64", "windows_x64", "macos_arm64"}) {
            std::ifstream oracle_file(std::string(LEMON_TEST_FIXTURES) + "/llamacpp-oracle/" + view + ".json");
            const auto protocol = Json::parse(oracle_file).at("protocol");
            check(protocol.at("bindings")[i].at("operation") == kinds[i] && protocol.at("bindings")[i].at("models") == 511 &&
                protocol.at("bindings")[i].at("delivery") == (i >= 3 && i <= 5 ? 1 : 0), "FrozenBindingCell:" + std::string(view) + ":" + ids[i]);
            std::vector<std::string> events;
            for (const auto& hook : requests[i]) events.push_back(hook == "llamacpp.max_tokens_alias" ? "alias" : "sanitizer");
            const auto path = owner.semantics.at("transport").at("path").get<std::string>();
            if (i >= 3 && i <= 5) {
                events.push_back("stream:" + path);
                const auto& transcript = protocol.at("streaming").at(path + ":0:sse");
                check(transcript.at("events") == Json(events), "FrozenStreamingChain:" + std::string(view) + ":" + ids[i]);
                if (i == 4) {
                    const auto& whitespace = protocol.at("streaming").at(path + ":1:sse");
                    check(whitespace.at("forwarded").at("body") == whitespace.at("input"), "StreamingCompletionOriginalBytes:" + std::string(view));
                }
            } else {
                const auto& transcript = protocol.at("buffered").at(std::to_string(kinds[i]) + ":0");
                events.push_back(transcript.at("forwarded").at("method").get<std::string>() + ":" + transcript.at("forwarded").at("path").get<std::string>());
                if (!responses[i].empty()) events.push_back("normalization");
                check(transcript.at("events") == Json(events), "FrozenBufferedChain:" + std::string(view) + ":" + ids[i]);
            }
        }
    }
    auto endpoint_source = source();
    endpoint_source["merge"]["operations"][6]["transport"]["path"] = "/v1/chat/completions";
    auto endpoint_policy = policy;
    endpoint_policy.owners[6].semantics["transport"]["path"] = "/v1/chat/completions";
    endpoint_policy.owners[0].collisions = {"chat.streaming", "embeddings.buffered", "slot_action.buffered"};
    endpoint_policy.owners[3].collisions = {"chat.buffered", "embeddings.buffered", "slot_action.buffered"};
    endpoint_policy.owners[6].collisions = {"chat.buffered", "chat.streaming", "slot_action.buffered"};
    pin_source(endpoint_policy, endpoint_source.dump());
    check(bool(authorize({child()}, endpoint_policy, endpoint_source.dump()).definition), "InheritedDifferentKindEndpointSiblingAdmitted");

    auto value = child();
    value["edits"] = Json::array({addition(alias())});
    reject("NewHooklessAliasRejected", {value}, "protected_owner_recreated");
    value["edits"] = Json::array({addition(alias(0, true))});
    reject("CopiedSanitizerNewOwnerRejected", {value}, "authored_hook_selection");
    auto removed = child("removed");
    removed["edits"] = Json::array({edit("remove", "chat.buffered")});
    auto descendant = child("descendant", "removed");
    check(bool(authorize({removed, descendant}).definition), "RemoveBufferedRetainIndependentStreamingSibling");
    descendant["edits"] = Json::array({addition(alias())});
    reject("RemovedOwnerNewHooklessReplacementRejected", {removed, descendant}, "protected_owner_recreated");
    descendant["edits"] = Json::array({addition(alias(0, true))});
    reject("RemovedOwnerCopiedSanitizerRejected", {removed, descendant}, "authored_hook_selection");
    auto same_layer = removed;
    same_layer["edits"].push_back(addition(alias()));
    reject("RemoveRenameSameLayerRejected", {same_layer}, "protected_owner_recreated");
    same_layer["edits"][1] = addition(alias(0, true));
    reject("RemoveRenameWithHookSameLayerRejected", {same_layer}, "authored_hook_selection");
    auto hook_free = source()["merge"]["operations"][4];
    hook_free["request"].erase("hooks");
    hook_free.erase("response");
    removed["edits"] = Json::array({edit("remove", "completion.streaming")});
    check(bool(authorize({removed, child("middle", "removed"), child("leaf", "middle")}).definition), "HookFreeRemovalTombstonePersists");
    descendant["edits"] = Json::array({addition(hook_free)});
    reject("HookFreeRemovedOwnerCannotReappear", {removed, descendant}, "authored_owner_change");
    same_layer = removed;
    same_layer["edits"].push_back(addition(hook_free));
    reject("IdenticalHookFreeRemoveReaddRejected", {same_layer}, "authored_owner_change");
    value = child();
    auto replacement = edit("replace", "completion.streaming");
    replacement["value"] = hook_free;
    value["edits"] = Json::array({replacement});
    reject("IdenticalHookFreeOwnerReauthoringRejected", {value}, "authored_owner_change");
    value = child();
    auto hidden_write = edit("replace", "completion.streaming", "/transport/path");
    hidden_write["value"] = "/temporary-owner-write";
    value["edits"] = Json::array({hidden_write, edit("remove", "completion.streaming")});
    reject("HiddenHookFreeOwnerWriteThenRemovalRejected", {value}, "authored_owner_change");
    for (const auto& field : policy.owners[4].sealed_fields) if (hook_free.contains(field) && field != "id") {
        value = child();
        auto write = edit("replace", "completion.streaming", "/" + field);
        write["value"] = hook_free[field];
        value["edits"] = Json::array({write});
        reject("HookFreeSealedWrite:" + field, {value}, "authored_owner_change");
    }
    auto optional = policy;
    for (auto& slot : optional.slots) slot.removal = SlotRemoval::Independent;
    repin(optional);
    auto diminished = child("diminished");
    diminished["edits"] = Json::array();
    for (const auto& owner : optional.owners) for (const auto* stage : {"request", "response"})
        diminished["edits"].push_back(edit("clear", owner.id, "/" + std::string(stage) + "/hooks"));
    check(bool(authorize({diminished, child("leaf", "diminished")}, optional).definition), "AllGrantsRemovedOwnersRemain");
    auto attack = child("leaf", "diminished");
    attack["edits"] = Json::array({addition(alias())});
    reject("AliasObligationsSurviveZeroGrants", {diminished, attack}, "protected_owner_recreated", optional);
    auto fixed = policy;
    fixed.owners[4].allow_removal = false;
    repin(fixed);
    reject("HookFreeOwnerRemovalNeedsPermission", {removed}, "mandatory_owner_removed", fixed);
    auto dependency = policy;
    dependency.owners[4].required_owners = {"chat.buffered"};
    repin(dependency);
    check(bool(authorize({child()}, dependency).definition), "OwnerDependencySatisfied");
    value = child(); value["edits"] = Json::array({edit("remove", "chat.buffered")});
    reject("RetainedOwnerDependencyCannotDisappear", {value}, "required_owner_removed", dependency);

    audit_reject("HookFreeOwnerRecordRequired", "uncovered_operation_owner", [](auto& c) { c.owners.erase(c.owners.begin() + 4); });
    audit_reject("OwnerIdentityUnique", "duplicate_operation_owner", [](auto& c) { c.owners.push_back(c.owners[4]); });
    audit_reject("OwnerRecordMustExist", "missing_operation_owner", [](auto& c) { c.owners[4].id = "absent"; });
    audit_reject("OwnerSemanticsAudited", "operation_owner_semantics_mismatch", [](auto& c) { c.owners[4].semantics["delivery"] = "buffered"; });
    audit_reject("EmptyRequestChainAudited", "owner_chain_mismatch", [](auto& c) { c.owners[4].request_hooks = {"llamacpp.max_tokens_alias"}; });
    audit_reject("EmptyResponseChainAudited", "owner_chain_mismatch", [](auto& c) { c.owners[4].response_hooks = {"llamacpp.client_model"}; });
    audit_reject("EmptyHandlerChainAudited", "owner_chain_mismatch", [](auto& c) { c.owners[4].handler_hooks = {"moonshine.transcription"}; });
    audit_reject("OwnerSealedFieldsComplete", "invalid_owner_fields", [](auto& c) { c.owners[4].sealed_fields.pop_back(); });
    audit_reject("CollisionGraphComplete", "owner_collisions_mismatch", [](auto& c) { c.owners[4].collisions.clear(); });
    audit_reject("OwnerDependencyMustExist", "invalid_owner_dependency", [](auto& c) { c.owners[4].required_owners = {"absent"}; });
    audit_reject("OwnerCannotDependOnItself", "invalid_owner_dependency", [](auto& c) { c.owners[4].required_owners = {c.owners[4].id}; });
    audit_reject("OwnerDependencyUnique", "invalid_owner_dependency", [](auto& c) { c.owners[4].required_owners = {c.owners[0].id, c.owners[0].id}; });
    audit_reject("OwnerDependenciesAcyclic", "owner_dependency_cycle", [](auto& c) { c.owners[4].required_owners = {c.owners[0].id}; c.owners[0].required_owners = {c.owners[4].id}; });
    audit_reject("OwnerSlotRemovalConsistent", "invalid_owner_removal", [](auto& c) { c.owners[0].allow_removal = false; });
    audit_reject("ForbiddenSlotMakesOwnerNonremovable", "invalid_owner_removal", [](auto& c) { c.slots[0].removal = SlotRemoval::Forbidden; });
    audit_reject("IndependentOwnerGraphPin", "owner_graph_digest_mismatch", [](auto& c) { c.owner_graph_digest.back() ^= 1; c.policy_digest = v2::export_policy_digest(c); }, false);
    audit_reject("HookFreeOwnerCannotBeMutable", "mutable_owner_semantics", [](auto& c) {
        c.slots[0].mutable_inputs[0].target = {"/operations", RecordKey{RecordKey::Field::Id, "completion.streaming"}, "/delivery"};
    });
    check(raw_graph_error([](auto&, auto&, auto&) {}, true) == "owner_provenance_changed", "FinalHookFreeOwnerProvenance");
    check(raw_graph_error([](auto& d, auto&, auto&) { d.declaration["operations"][4]["delivery"] = "buffered"; }) == "owner_semantics_changed", "FinalHookFreeOwnerSemantics");
    check(raw_graph_error([](auto&, auto& p, auto&) { p.declaration["operations"].erase(p.declaration["operations"].begin() + 4); }, false, true) == "owner_reintroduced", "FinalImmediateParentOwnerSubset");
    check(raw_graph_error([](auto& d, auto&, auto& c) { d.declaration["operations"].erase(d.declaration["operations"].begin() + 4); c.owners[4].allow_removal = false; }) == "mandatory_owner_removed", "FinalHookFreeRemovalPermission");
    check(raw_graph_error([](auto& d, auto&, auto&) { auto op = d.declaration["operations"][4]; op["id"] = "renamed"; d.declaration["operations"].push_back(op); }) == "protected_owner_recreated", "FinalAliasWithoutAnyHookGrants");
    check(final_chain_error() == "owner_chain_changed", "FinalOwnerChainMustMatchGraph");
    auto changed_abis = installed_hook_abis();
    ++changed_abis[0].implementation_revision;
    check(has_code(v2::audit_export_contracts({}, nullptr, changed_abis, installed_builtin_exports().digest()), "registry_digest_mismatch"), "EmptyExportRegistryStillPinsAbiMetadata");
    {
        DefinitionLimits bounded;
        bounded.retained_logical_bytes = 0;
        std::size_t bytes = 0;
        v2::Budget limited{bounded, {}, bytes};
        bool rejected = false;
        try { v2::operation_owner_semantics(policy.owners[0].semantics, limited); }
        catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected && bytes == 0, "OwnerSemanticsChargedBeforeCopy");
    }
    {
        DefinitionLimits bounded;
        bounded.work_nodes = 0;
        std::size_t bytes = 0;
        v2::Budget limited{bounded, {}, bytes};
        bool rejected = false;
        try { v2::operation_hook_chain(source()["merge"]["operations"][0], "request", limited); }
        catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected && bytes == 0, "OwnerChainWorkChargedBeforeRetention");
    }
    auto tight = limits; tight.work_nodes = 1;
    check(!authorize({child()}, policy, read_bytes("export.siblings.synthetic.json"), tight).definition, "OwnerGraphWorkBoundFailsClosed");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
