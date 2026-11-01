#include "backend_definition_binding_fixture.h"
#include "backend_definition_runner_internal.h"
#include "configured_v2_backend_ops_internal.h"
#include "lemon/auto_tune.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/llamacpp/llamacpp_request.h"
#include "lemon/utils/json_utils.h"

#include <filesystem>
#include <fstream>
#include <thread>

using namespace binding_fixture;
using namespace lemon;

namespace {
Fixture candidate(Json doc, bool granted = true) {
    const auto id = doc.at("id").get<std::string>();
    auto declaration = doc.at("merge");
    declaration["recipe"] = doc.at("recipe");
    const auto semantics = container_execution_owner(declaration);
    const BuiltinContainerExecutionOwner owner{{id, 1}, semantics, v2::identity("lemonade.backend-container-owner.v1\n", semantics)};
    const auto admitted = admit({}, builtins(false, &doc, false, granted ? &owner : nullptr));
    if (!admitted.catalog) { diagnostics(admitted.diagnostics); throw std::logic_error("C7a fixture admission"); }
    const auto token = admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, id, 1});
    const auto compiled = compile_execution_definition(token);
    if (granted && !compiled.definition) diagnostics(compiled.diagnostics);
    return {admitted.catalog, token, compiled.definition};
}
std::shared_ptr<const ExecutableDefinition> executable(const Fixture& value) {
    if (!value.compiled) throw std::logic_error("C7a fixture compilation");
    const auto bound = bind(value);
    if (!bound.definition) { diagnostics(bound.diagnostics); throw std::logic_error("C7a fixture binding"); }
    return bound.definition;
}
template<class Call> bool refuses(Call call) {
    try { call(); } catch (const std::exception&) { return true; }
    return false;
}
void stream_contract() {
    auto doc = Json::parse(read_bytes("c7a.stream.json")).at("definition");
    for (const auto* id : {"rocmfpx", "llamacpp-halobox", "llamacpp-nathanw"}) {
        doc["id"] = id;
        doc["recipe"] = std::string(id) == "rocmfpx" ? "rocmfpx" : "llamacpp";
        const auto value = candidate(doc);
        const auto bound = executable(value);
        DefinitionRunner runner(bound);
        ExecutionSession session{bound};
        httplib::Server http;
        std::string wire_path, wire_body;
        http.Post("/.*", [&](const auto& request, auto& response) {
            wire_path = request.target; wire_body = request.body;
            response.set_content("data: [DONE]\n\n", "text/event-stream");
        });
        const auto port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("C7a loopback bind");
        std::thread listener([&] { http.listen_after_bind(); });
        struct Cleanup { httplib::Server& server; std::thread& thread; ~Cleanup() { server.stop(); thread.join(); } } cleanup{http, listener};
        http.wait_until_ready();
        for (const auto* path : {"/v1/chat/completions?probe=1", "/v1/responses?probe=1", "/inherited/arbitrary", "/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
            for (const auto* body : {"{broken", "{\"max_completion_tokens\":7,\"messages\":[]}"}) {
                std::string expected = body;
                if (std::string(path) == "/v1/chat/completions" || std::string(path) == "/v1/responses") {
                    auto parsed = Json::parse(body, nullptr, false);
                    if (!parsed.is_discarded()) {
                        if (std::string(path) == "/v1/chat/completions") utils::JsonUtils::add_legacy_max_tokens_alias(parsed);
                        expected = backends::llamacpp::sanitize_tool_schema_limits(std::move(parsed)).dump();
                    }
                }
                const auto mapped = runner.map_stream(path, ModelType::LLM, session, body, false, 37);
                check(mapped.path == path && mapped.sse == false && mapped.timeout_seconds == 37,
                    "C7aInheritedStreamControls:" + std::string(id) + path);
                httplib::Client client("127.0.0.1", port);
                client.set_connection_timeout(2, 0); client.set_read_timeout(2, 0);
                const auto response = client.Post(mapped.path, mapped.original_body.value_or(mapped.body.dump()), "application/json");
                check(response && response->body == "data: [DONE]\n\n" && wire_path == path && wire_body == expected,
                    "C7aExactStackStreamWire:" + std::string(id) + path + body);
            }
        }
        for (const auto& path : {std::string("//foreign/path"), std::string("https://foreign/path"), std::string("/x\r\nheader"), std::string("/x\0y", 4)})
            check(refuses([&] { runner.map_stream(path, ModelType::LLM, session, "{}", true, 1); }), "C7aStreamEndpointConfinement");
        auto changed_doc = doc;
        changed_doc["merge"]["launch"]["direct_stream_policy"] = "bound_passthrough";
        auto declaration = changed_doc.at("merge"); declaration["recipe"] = changed_doc.at("recipe");
        check(!AuthorityInspection::permits_execution(*value.token, declaration), "C7aStreamPolicySealed");
        declaration = value.compiled->normalized_declaration();
        declaration["operations"][0]["transport"]["path"] = "/other";
        check(!AuthorityInspection::permits_execution(*value.token, declaration), "C7aStreamOperationsSealed");
        check(!candidate(doc, false).compiled, "C7aUngrantedIdentityCannotStream");
        const auto narrow = executable(candidate(changed_doc));
        DefinitionRunner narrow_runner(narrow);
        ExecutionSession narrow_session{narrow};
        check(refuses([&] { narrow_runner.map_stream("/inherited/arbitrary", ModelType::LLM, narrow_session, "{}", true, 1); }),
            "C7aUnrequestedEndpointUnreachable");
    }
    auto external = root("c7a-external");
    external["merge"]["launch"]["direct_stream_policy"] = "builtin_container_legacy";
    const auto external_admitted = admit({external}, empty_builtins());
    check(bool(external_admitted.catalog), "C7aExternalLocalAdmission");
    if (external_admitted.catalog) {
        const auto compiled = compile_execution_definition(external_admitted.catalog->v2_definition("c7a-external"));
        check(!compiled.definition && has_code(compiled.diagnostics, "unsupported_execution_feature"), "C7aExternalCannotStream");
    }
    auto local = root("c7a-local");
    local["merge"]["launch"]["direct_stream_policy"] = "builtin_container_legacy";
    const auto local_admitted = admit({}, builtins(false, &local));
    check(bool(local_admitted.catalog), "C7aLocalBuiltinAdmission");
    if (local_admitted.catalog) check(!compile_execution_definition(local_admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "c7a-local", 1})).definition,
        "C7aLocalBuiltinCannotStream");
}
}

#include "c7a_context_budget.h"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { allow_entries = true; stream_contract(); load_log_contract(); context_budget_contract(); }
    catch (const std::exception& error) { std::printf("C7a exception: %s\n", error.what()); check(false, "C7aPipeline"); }
    return finish();
}
