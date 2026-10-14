#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/configured/request_mapper.h"
#include "backend_definition_resources.h"
#include "thinksound_oracle.h"

#include <functional>

using namespace thinksound_oracle;
using namespace lemon::backends;

int main() {
    BackendDefinitionStore store;
    const DefinitionSource source{"thinksound.json", k_thinksound_definition, DefinitionOrigin::Builtin};
    const auto valid = store.validate(source);
    for (const auto& diagnostic : valid.diagnostics) std::printf("%s %s %s\n", diagnostic.code.c_str(), diagnostic.pointer.c_str(), diagnostic.message.c_str());
    check(valid.definition && valid.diagnostics.empty(), "complete ThinkSound definition compiles");
    if (!valid.definition) return 1;
    const auto& program = valid.definition->operations.front();
    RequestMapper mapper;
    for (const auto& [input, expected] : mapping_cases) {
        check(mapper.map(program, {}, input).body.dump() == expected, "compiled mapper matches hand-authored A wire literal");
    }
    check(program.path == "/generate" && program.port == "http" && program.kind == "http_bytes" && program.codec == "raw_bytes.v1" && program.timeout_seconds == 600,
          "independent resolved transport metadata");
    for (const auto& prompt : {json(nullptr), json(false), json(4), json::array(), json::object()}) {
        bool rejected = false;
        try { mapper.map(program, {}, json{{"prompt", prompt}}); } catch (const json::type_error&) { rejected = true; }
        check(rejected, "compiled typed reference preserves prompt type error");
    }
    const json baseline = json::parse(source.text);
    auto malformed = [&](const std::function<void(json&)>& mutation, const std::string& code, const std::string& pointer) {
        auto doc = baseline;
        mutation(doc);
        auto result = store.validate({"malformed.json", doc.dump(), DefinitionOrigin::Builtin});
        bool found = false;
        for (const auto& error : result.diagnostics) found |= error.code == code && error.pointer == pointer;
        check(!result.definition && found, "malformed diagnostic " + code + " " + pointer);
    };
    malformed([](auto& doc) { doc["schema_version"] = true; }, "invalid_type", "/schema_version");
    malformed([](auto& doc) { doc["schema_version"] = 2; }, "invalid_value", "/schema_version");
    malformed([](auto& doc) { doc["weird~/key"] = 1; }, "unknown_key", "/weird~0~1key");
    malformed([](auto& doc) { doc["launch"]["program"]["script"] = "bad"; }, "unknown_key", "/launch/program/script");
    malformed([](auto& doc) { doc["operations"][0]["provenance"] = "LegacyForwarding"; }, "unknown_key", "/operations/0/provenance");
    malformed([](auto& doc) { doc["operations"][0]["transport"]["stream_codec"] = "absent.v1"; }, "unsupported_feature", "/operations/0/transport");
    malformed([](auto& doc) { doc["operations"][0]["delivery"] = "streaming"; }, "unsupported_feature", "/operations/0");
    malformed([](auto& doc) { doc["operations"].push_back(doc["operations"][0]); }, "operation_overlap", "/operations/1");
    malformed([](auto& doc) { doc["hooks"]["launch"] = {{"id", "legacy_non_chat_sse"}, {"params", json::object()}}; }, "unknown_hook", "/hooks/launch/id");
    malformed([](auto& doc) { doc["operations"][0].erase("transport"); doc["operations"][0]["handler"] = {{"id", "absent.v1"}, {"params", json::object()}}; }, "unknown_handler", "/operations/0/handler/id");
    malformed([](auto& doc) { doc["launch"]["prepare_order"] = {"backend", "ports", "program", "artifacts"}; }, "invalid_relationship", "/launch/prepare_order");
    malformed([](auto& doc) { doc["launch"]["watchdog"]["path"] = "/other"; }, "invalid_relationship", "/launch/watchdog");
    malformed([](auto& doc) { doc["install"]["cases"].push_back(doc["install"]["cases"][0]); }, "selector_overlap", "/install/cases/2");
    malformed([](auto& doc) { doc["operations"][0]["request"]["set"]["/caption/child"] = 1; }, "mapping_overlap", "/operations/0/request/set/~1caption~1child");
    malformed([](auto& doc) { doc["operations"][0]["request"]["set"]["/caption"] = {{"ref", "/secret/key"}}; }, "invalid_reference", "/operations/0/request/set/~1caption/ref");
    malformed([](auto& doc) { doc["operations"][0]["request"]["set"]["/caption"] = {{"ref", "/request/~invalid"}}; }, "invalid_pointer", "/operations/0/request/set/~1caption/ref");
    malformed([](auto& doc) { doc["operations"][0]["request"]["set"]["/caption"] = {{"ref", "/request/value"}, {"type", "string"}, {"default", nullptr}}; }, "invalid_type", "/operations/0/request/set/~1caption/default");
    malformed([](auto& doc) { doc["metadata"]["options"].push_back(doc["metadata"]["options"][0]); }, "option_collision", "/metadata/options/1/name");
    malformed([](auto& doc) { doc["metadata"]["config_extra"]["backend"] = false; }, "invalid_type", "/metadata/config_extra/backend");
    malformed([](auto& doc) { doc["metadata"]["supported_modes"] = {"unknown-mode"}; }, "invalid_value", "/metadata/supported_modes");
    malformed([](auto& doc) { auto& option = doc["metadata"]["options"][0]; option["value_type"] = "integer"; option["default_value"] = 2; option["enum"] = {1,2,3}; option["minimum"] = 1; option["maximum"] = 2; option.erase("ui"); }, "invalid_value", "/metadata/options/0/enum");
    auto duplicate = source.text;
    duplicate.insert(duplicate.find('{') + 1, "\"id\":\"duplicate\",");
    check(!store.validate({"duplicate.json", duplicate, DefinitionOrigin::Builtin}).definition, "duplicate keys fail");
    const auto nested_duplicate = store.validate({"nested.json", R"({"schema_version":1,"id":"fixture","metadata":{"name":1,"name":2}})", DefinitionOrigin::Builtin});
    check(std::any_of(nested_duplicate.diagnostics.begin(), nested_duplicate.diagnostics.end(), [](const auto& error) { return error.code == "duplicate_key" && error.pointer == "/metadata/name"; }), "duplicate nested pointer");
    {
        auto doc = baseline;
        doc["operations"][0]["request"] = {{"base", "empty"}, {"set", {{"/a~1b/~0c", {{"ref", "/request/a~1b/~0c"}}}}}};
        const auto escaped = store.validate({"escaped.json", doc.dump(), DefinitionOrigin::Builtin});
        check(escaped.definition && mapper.map(escaped.definition->operations[0], {}, {{"a/b", {{"~c", false}}}}).body == json{{"a/b", {{"~c", false}}}}, "escaped source and destination pointers");
        doc["operations"][0]["request"] = {{"base", "identity"}, {"remove", {"/drop"}}, {"set", {{"/caption", {{"ref", "/request/prompt"}}}, {"/echo", {{"ref", "/request/caption"}}}}}};
        const auto immutable = store.validate({"immutable.json", doc.dump(), DefinitionOrigin::Builtin});
        check(immutable.definition && mapper.map(immutable.definition->operations[0], {}, {{"prompt", "new"}, {"caption", "old"}, {"drop", nullptr}}).body == json{{"prompt", "new"}, {"caption", "new"}, {"echo", "old"}}, "mapping expressions use immutable input after remove");
    }
    check(!store.validate({"external.json", source.text, DefinitionOrigin::External}).definition, "external named CLI flag rejected");
    check(!store.validate({"huge.json", std::string(1024 * 1024 + 1, ' '), DefinitionOrigin::Builtin}).definition, "definition byte limit");
    malformed([](auto& doc) { json expression = true; for (int i = 0; i < 34; ++i) expression = {{"not", expression}}; doc["operations"][0]["request"]["set"]["/caption"] = expression; }, "limit_exceeded", "/operations/0/request/set/~1caption/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not/not");
    malformed([](auto& doc) { doc["operations"][0]["request"]["set"]["/caption"] = {{"coalesce", json(std::vector<json>(4097, nullptr))}}; }, "limit_exceeded", "/operations/0/request/set/~1caption/coalesce/4096");
    auto expression_program = [&](const json& expression) {
        auto doc = baseline;
        doc["operations"][0]["request"]["set"] = {{"/value", expression}};
        const auto result = store.validate({"expression.json", doc.dump(), DefinitionOrigin::Builtin});
        check(result.definition != nullptr, "expression compilation");
        if (!result.definition) throw std::runtime_error("Expression fixture failed");
        return result.definition;
    };
    for (const auto& value : {json(nullptr), json(0), json(false), json("")}) {
        const auto definition = expression_program({{"coalesce", json::array({json{{"ref", "/request/value"}}, "fallback"})}});
        check(mapper.map(definition->operations[0], {}, {{"value", value}}).body["value"] == value, "coalesce preserves present value");
        check(mapper.map(definition->operations[0], {}, json::object()).body["value"] == "fallback", "coalesce uses absent fallback");
    }
    for (const auto& [expression, expected] : std::vector<std::pair<json, json>>{
             {json{{"all", json::array({false, json{{"ref", "/request/absent"}, {"type", "boolean"}}})}}, false},
             {json{{"any", json::array({true, json{{"ref", "/request/absent"}, {"type", "boolean"}}})}}, true},
             {json{{"present", json{{"ref", "/request/value"}}}}, true},
             {json{{"string", json{{"ref", "/request/value"}}}}, "false"},
             {json{{"literal", "${request.value}"}}, "${request.value}"}}) {
        const auto definition = expression_program(expression);
        check(mapper.map(definition->operations[0], {}, {{"value", false}}).body["value"] == expected, "literal/short-circuit expression semantics");
    }
    auto identity_doc = baseline;
    auto& op = identity_doc["operations"][0];
    op["operation"] = "chat_completion"; op["delivery"] = "streaming"; op["model_modes"] = {"chat"};
    op["transport"]["kind"] = "http_sse"; op["transport"]["stream_codec"] = "openai_sse.v1";
    op["request"] = {{"base", "identity"}};
    const auto identity = store.validate({"identity.json", identity_doc.dump(), DefinitionOrigin::Builtin});
    const std::string original = "{ \"z\": 1, \"a\": 0 }";
    check(identity.definition && mapper.map(identity.definition->operations[0], {}, json::parse(original), original).original_body == original, "identity SSE preserves original bytes");
    const auto artifact_hook = +[](const BackendDefinition&, const ModelInfo&, const CheckpointResolveContext&, const json&) { return std::string("fixture"); };
    const json parameters = {{"type", "object"}, {"additionalProperties", false}};
    BackendHookRegistry hooks({{"fixture.v1", HookStage::Artifacts, parameters, {}, artifact_hook}});
    check(hooks.require("fixture.v1", HookStage::Artifacts).id == "fixture.v1", "typed hook table resolution");
    for (int invalid = 0; invalid < 4; ++invalid) {
        bool rejected = false;
        try {
            if (invalid == 0) hooks.require("fixture.v1", HookStage::Launch);
            if (invalid == 1) hooks.require("absent.v1", HookStage::Artifacts);
            if (invalid == 2) BackendHookRegistry({{"fixture.v1", HookStage::Launch, parameters, {}, artifact_hook}});
            if (invalid == 3) BackendHookRegistry({{"fixture.v1", HookStage::Artifacts, parameters, {}, ArtifactHook(nullptr)}});
        } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "unknown/null/wrong hook stage/signature rejected");
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
