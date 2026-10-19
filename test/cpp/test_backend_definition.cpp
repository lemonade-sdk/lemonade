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
    const auto artifact_hook = +[]() { return default_backend_ops(); };
    BackendHookRegistry hooks({{"fixture.v1", HookStage::Artifacts, artifact_hook}});
    check(hooks.require("fixture.v1", HookStage::Artifacts).id == "fixture.v1" && hooks.target<ArtifactHook>("fixture.v1", HookStage::Artifacts)() == default_backend_ops(), "typed hook table resolution");
    for (int invalid = 0; invalid < 5; ++invalid) {
        bool rejected = false;
        try {
            if (invalid == 0) hooks.require("fixture.v1", HookStage::Request);
            if (invalid == 1) hooks.require("absent.v1", HookStage::Artifacts);
            if (invalid == 2) BackendHookRegistry({{"fixture.v1", HookStage::Request, artifact_hook}});
            if (invalid == 3) BackendHookRegistry({{"fixture.v1", HookStage::Artifacts, ArtifactHook(nullptr)}});
            if (invalid == 4) BackendHookRegistry({{"fixture.v1", HookStage::Artifacts, artifact_hook}, {"fixture.v1", HookStage::Artifacts, artifact_hook}});
        } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "unknown/null/duplicate/wrong hook stage/signature rejected");
    }
    const auto& manifest = installed_hook_manifest();
    const auto& installed = installed_backend_hooks().registrations();
    const std::vector<std::pair<std::string, std::string>> expected_hooks = {{"llamacpp.gguf", "artifacts"}, {"llamacpp.max_tokens_alias", "request"},
        {"llamacpp.tool_schema_limits", "request"}, {"llamacpp.client_model", "response"}, {"kokoro.voice_index", "launch"},
        {"moonshine.model_arch", "launch"}, {"moonshine.transcription", "operation"}, {"onnxruntime.model_directory", "artifacts"},
        {"onnxruntime.servable_directory", "launch"}, {"onnxruntime.classify", "operation"}, {"onnxruntime.startup_error", "start_failure"}};
    bool agree = manifest.size() == installed.size() && manifest.size() == expected_hooks.size();
    for (size_t i = 0; agree && i < manifest.size(); ++i) {
        agree = manifest[i].id == installed[i].id && manifest[i].stage == hook_stage_name(installed[i].stage) && manifest[i].id == expected_hooks[i].first && manifest[i].stage == expected_hooks[i].second;
    }
    check(agree, "validator hook manifest names exactly the installed compiled hooks and stages");
    json features = json::array();
    for (const auto& hook : manifest) features.push_back(hook.stage + ":" + hook.id + (hook.operation.empty() ? "" : ":" + hook.operation));
    check(installed_definition_features()["hooks"] == features, "feature manifest lists every installed hook");
    const auto request_hook = [&](const json& selection) { return [selection](auto& doc) { doc["operations"][0]["request"]["hooks"] = json::array({selection}); }; };
    malformed(request_hook({{"id", "absent.v1"}, {"params", json::object()}}), "unknown_hook", "/operations/0/request/hooks/0/id");
    malformed(request_hook({{"id", "llamacpp.client_model"}, {"params", json::object()}}), "unknown_hook", "/operations/0/request/hooks/0/id");
    malformed(request_hook({{"id", "llamacpp.tool_schema_limits"}, {"params", {{"x", 1}}}}), "invalid_value", "/operations/0/request/hooks/0/params");
    malformed([](auto& doc) { doc["operations"][0]["response"]["hooks"] = json::array({{{"id", "llamacpp.client_model"}, {"params", json::object()}}}); }, "invalid_relationship", "/operations/0/response/hooks");
    malformed([](auto& doc) { doc["artifacts"]["main"]["hook"] = {{"id", "llamacpp.gguf"}, {"params", json::object()}}; }, "invalid_relationship", "/artifacts/main/hook");
    malformed([](auto& doc) { doc["artifacts"]["main"]["strategy"] = "hook"; doc["artifacts"]["main"]["hook"] = {{"id", "llamacpp.gguf"}, {"params", json::object()}}; }, "invalid_relationship", "/artifacts/main/hook");
    malformed([](auto& doc) { doc["artifacts"]["main"]["strategy"] = "hook"; doc["artifacts"]["main"]["hook"] = {{"id", "llamacpp.max_tokens_alias"}, {"params", json::object()}}; }, "unknown_hook", "/artifacts/main/hook/id");
    malformed([](auto& doc) { doc["launch"]["prepare_order"] = {"artifacts", "backend", "install", "program", "ports"}; }, "invalid_relationship", "/launch/prepare_order");
    malformed([](auto& doc) { doc["launch"]["program"]["source"] = "managed"; doc["launch"]["prepare_order"] = {"artifacts", "backend", "program", "install", "ports"}; }, "invalid_relationship", "/launch/prepare_order");
    malformed([](auto& doc) { doc["launch"]["program"]["source"] = "managed"; doc["launch"]["prepare_order"] = {"artifacts", "backend", "ports", "install", "program"}; }, "invalid_relationship", "/launch/prepare_order");
    malformed([](auto& doc) { doc["launch"]["prepare_order"] = {"artifacts", "backend", "program"}; }, "invalid_value", "/launch/prepare_order");
    {
        auto doc = baseline;
        doc["launch"]["program"]["source"] = "managed";
        doc["launch"]["prepare_order"] = {"backend", "install", "artifacts", "ports", "program"};
        check(store.validate({"install.json", doc.dump(), DefinitionOrigin::Builtin}).definition != nullptr, "a separate managed install may precede ports and a later program lookup");
        auto& slots = doc["operations"][0];
        slots = {{"operation", "slots"}, {"delivery", "buffered"}, {"model_modes", {"*"}}, {"request", {{"base", "identity"}}},
            {"transport", {{"kind", "http_json"}, {"method", "GET"}, {"path", "/slots"}, {"port", "http"}, {"timeout", "global"}, {"response_codec", "json.v1"}}}};
        check(store.validate({"slots.json", doc.dump(), DefinitionOrigin::Builtin}).definition != nullptr, "slots bind a GET JSON transport");
        slots["transport"]["method"] = "POST";
        check(!store.validate({"slots.json", doc.dump(), DefinitionOrigin::Builtin}).definition, "slots reject a POST transport");
        slots["operation"] = "tokenize";
        check(store.validate({"tokenize.json", doc.dump(), DefinitionOrigin::Builtin}).definition != nullptr, "tokenize binds a POST JSON transport");
        slots["transport"]["method"] = "GET";
        check(!store.validate({"tokenize.json", doc.dump(), DefinitionOrigin::Builtin}).definition, "tokenize rejects a GET transport");
    }
    malformed([](auto& doc) { doc["launch"]["prepare_order"] = {"artifacts", "backend", "hook", "program", "ports"}; }, "invalid_relationship", "/launch/prepare_order");
    malformed([](auto& doc) { doc["hooks"]["launch"] = {{"id", "kokoro.voice_index"}, {"params", json::object()}}; }, "invalid_relationship", "/launch/prepare_order");
    malformed([](auto& doc) { doc["launch"]["prepare_order"] = {"artifacts", "backend", "hook", "program", "ports"}; doc["hooks"]["launch"] = {{"id", "llamacpp.gguf"}, {"params", json::object()}}; }, "unknown_hook", "/hooks/launch/id");
    malformed([](auto& doc) { doc["artifacts"]["main"]["cache_resolution"] = "sentinel_file"; }, "invalid_relationship", "/artifacts/main/sentinel");
    malformed([](auto& doc) { auto speech = doc["operations"][0]; speech["operation"] = "text_to_speech"; speech["model_modes"] = {"tts"}; doc["operations"].push_back(speech); }, "unsupported_feature", "/operations/1");
    malformed([](auto& doc) { doc["unsupported_messages"] = {{"embeddings", "no"}}; }, "unknown_key", "/unsupported_messages/embeddings");
    malformed([](auto& doc) { doc["launch"]["backend"] = {{"fixed", {{"linux", "cpu"}}}}; }, "invalid_shape", "/launch/backend");
    malformed([](auto& doc) { doc["launch"]["ports"][0]["span"] = 3; }, "invalid_relationship", "/launch/ports/0");
    malformed([](auto& doc) { doc["launch"]["ports"].push_back({{"name", "tcp"}, {"strategy", "offset"}, {"base", "http"}, {"offset", 2}, {"reject_zero", false}, {"error", ""}}); }, "invalid_reference", "/launch/ports/1/base");
    malformed([](auto& doc) { doc["launch"]["ports"][0]["strategy"] = "probe"; doc["launch"]["ports"][0]["span"] = 2; doc["launch"]["ports"].push_back({{"name", "tcp"}, {"strategy", "offset"}, {"base", "http"}, {"offset", 2}, {"reject_zero", false}, {"error", ""}}); }, "invalid_reference", "/launch/ports/1/base");
    malformed([](auto& doc) { doc["operations"][0]["transport"]["port"] = "other"; doc["launch"]["ports"].push_back({{"name", "other"}, {"reject_zero", false}, {"error", ""}}); }, "unsupported_feature", "/operations/0/transport/port");
    malformed([](auto& doc) { auto& op = doc["operations"][0]; op = {{"operation", "streaming_transcription"}, {"delivery", "realtime"}, {"model_modes", {"*"}}, {"request", {{"base", "identity"}}},
        {"transport", {{"kind", "realtime"}, {"method", "POST"}, {"path", "/"}, {"port", "http"}, {"timeout", "global"}, {"stream_codec", "jsonl.v1"}}}}; }, "unsupported_feature", "/operations/0/transport");
    malformed([](auto& doc) { auto& op = doc["operations"][0]; op.erase("transport"); op["operation"] = "transcription"; op["model_modes"] = {"*"}; op["handler"] = {{"id", "onnxruntime.classify"}, {"params", json::object()}}; }, "unknown_handler", "/operations/0/handler/id");
    malformed([](auto& doc) { doc["artifacts"]["main"]["files_from"] = "variant_directory"; doc["artifacts"]["main"]["files_error"] = "none"; }, "invalid_relationship", "/artifacts/main/files_from");
    malformed([](auto& doc) { doc["artifacts"]["main"]["files_error"] = "none"; }, "invalid_relationship", "/artifacts/main/files_error");
    malformed([](auto& doc) { doc["hooks"]["start_failure"] = {{"id", "kokoro.voice_index"}, {"params", json::object()}}; }, "unknown_hook", "/hooks/start_failure/id");
    {
        auto doc = baseline;
        doc["launch"]["ports"][0]["strategy"] = "probe";
        doc["launch"]["ports"][0]["span"] = 3;
        doc["launch"]["ports"].push_back({{"name", "tcp"}, {"strategy", "offset"}, {"base", "http"}, {"offset", 2}, {"reject_zero", false}, {"error", ""}});
        doc["operations"].push_back({{"operation", "streaming_transcription"}, {"delivery", "realtime"}, {"model_modes", {"*"}}, {"request", {{"base", "identity"}}},
            {"transport", {{"kind", "realtime"}, {"method", "TCP"}, {"path", "/"}, {"port", "tcp"}, {"timeout", "global"}, {"stream_codec", "jsonl.v1"}}}});
        doc["operations"].push_back({{"operation", "classification"}, {"delivery", "buffered"}, {"model_modes", {"*"}}, {"request", {{"base", "identity"}}}, {"handler", {{"id", "onnxruntime.classify"}, {"params", json::object()}}}});
        doc["hooks"]["start_failure"] = {{"id", "onnxruntime.startup_error"}, {"params", json::object()}};
        check(store.validate({"probe.json", doc.dump(), DefinitionOrigin::Builtin}).definition != nullptr, "probed span, offset port, realtime TCP transport, operation handler and start-failure hook validate");
    }
    {
        auto doc = baseline;
        auto& speech = doc["operations"][0];
        speech["operation"] = "text_to_speech"; speech["model_modes"] = {"tts"};
        auto streaming = speech;
        streaming["delivery"] = "streaming";
        doc["operations"].push_back(streaming);
        check(store.validate({"speech.json", doc.dump(), DefinitionOrigin::Builtin}).definition != nullptr, "speech binds buffered and streaming deliveries on one byte transport");
        doc["operations"][1]["transport"]["timeout"] = 5;
        check(!store.validate({"speech.json", doc.dump(), DefinitionOrigin::Builtin}).definition, "speech deliveries must share one transport");
        doc["operations"][1] = streaming;
        doc["operations"][1]["formats"] = json::array();
        check(!store.validate({"speech.json", doc.dump(), DefinitionOrigin::Builtin}).definition, "speech deliveries declare formats");
    }
    {
        const auto when = expression_program({{"when", json::array({json{{"present", json{{"ref", "/request/flag"}}}}, true})}});
        check(mapper.map(when->operations[0], {}, {{"flag", nullptr}}).body["value"] == true, "when yields its value for a present null");
        check(!mapper.map(when->operations[0], {}, json::object()).body.contains("value"), "when without its condition is omitted");
        const auto joined = expression_program({{"path_join", json::array({json{{"parent_path", json{{"ref", "/request/file"}}}}, json{{"ref", "/request/name"}}})}});
        check(mapper.map(joined->operations[0], {}, {{"file", "/models/dir/index.json"}, {"name", "net.onnx"}}).body["value"] == "/models/dir/net.onnx", "path_join of a parent path");
        check(mapper.map(joined->operations[0], {}, {{"file", "/models/dir/index.json"}, {"name", "/abs/net.onnx"}}).body["value"] == "/abs/net.onnx", "path_join keeps an absolute component");
        for (const auto& name : {json(nullptr), json(5)}) {
            std::string message;
            try { mapper.map(joined->operations[0], {}, {{"file", "/models/dir/index.json"}, {"name", name}}); } catch (const json::type_error& error) { message = error.what(); }
            check(message == std::string("[json.exception.type_error.302] type must be string, but is ") + name.type_name(), "path_join keeps the nlohmann path conversion error");
        }
        std::string message;
        try { mapper.map(joined->operations[0], {}, {{"file", "/models/dir/index.json"}}); } catch (const json::type_error& error) { message = error.what(); }
        check(message == "[json.exception.type_error.302] type must be string, but is null", "path_join converts a missing component as null");
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
