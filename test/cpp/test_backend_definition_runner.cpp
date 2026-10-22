#include "backend_runtime_fixture.h"

#include <future>

using namespace runtime_fixture;

int main() {
    try {
    Upstream upstream;
    ServerFixture fixture(upstream.port);
    auto& server = *fixture.server;
    entries.clear();
    const auto original = Json{{"model", "client-model"}, {"max_completion_tokens", 7}, {"marker", 4}};
    const auto chat = server.chat_completion(original);
    check(entries == std::vector<std::string>{"max_tokens_alias", "tool_schema_limits", "client_model"}, "RequestChainsFollowSealedOrder");
    check(chat.at("max_tokens") == 7 && chat.at("model") == "client-model" && chat.at("marker") == 4,
        "RequestChainsMatchOperationOwner: chat transforms through wrapped transport");
    check(original.contains("max_completion_tokens") && !original.contains("max_tokens"), "RequestDoesNotMutateCaller");
    bool response_cell = false;
    try {
        const auto completion = server.completion(original);
        response_cell = !completion.contains("max_tokens") && completion.at("model") == "upstream-model";
    } catch (const std::exception&) {}
    check(response_cell, "ResponseChainStaysOnSelectedCell");
    check(server.get_slots().at("slots") == true, "GetUsesWrappedTransport");
    std::string bytes;
    httplib::DataSink sink;
    sink.write = [&](const char* data, std::size_t size) { bytes.append(data, size); return true; };
    sink.is_writable = [] { return true; };
    sink.done = [] {};
    const std::string raw = "{ \"model\": \"client\", \"max_completion_tokens\": 2 }";
    server.forward_streaming_request("/v1/chat/completions", raw, sink, true, 1);
    check(upstream.stream_body == raw && bytes.find("[DONE]") != std::string::npos, "EmptyStreamingChainRemainsEmptyAndRaw");
    server.forward_streaming_request("/v1/chat/completions", "raw-body", sink, true, 1);
    check(upstream.stream_body == "raw-body", "PermittedRawStreamingBodySurvivesInvalidJson");
    std::vector<std::future<Json>> requests;
    for (int i = 0; i < 12; ++i) requests.push_back(std::async(std::launch::async, [&, i] {
        return server.chat_completion({{"model", std::to_string(i)}, {"max_completion_tokens", i + 1}});
    }));
    bool independent = true;
    for (int i = 0; i < 12; ++i) {
        const auto response = requests[i].get();
        independent &= response.at("model") == std::to_string(i) && response.at("max_tokens") == i + 1;
    }
    check(independent, "ConcurrentV2FramesAreIndependent");
    const auto executable = runtime_fixture::executable();
    DefinitionRunner runner(executable);
    const auto entry = ConfiguredBackendEntry::make(executable);
    Launch launch;
    auto info = model(); info.type = ModelType::EMBEDDING;
    RecipeOptions options; options.set_option("ctx_size", 1024);
    const auto prepared = runner.prepare_load({"model", info, options, "error", {}, {}}, entry->spec, launch);
    check(prepared.session->frame.values["launch"]["hook"]["context_size"] == 8192 && options.get_option("ctx_size") == 1024,
        "ContextFloorDoesNotMutateCaller");
    check(throws([&] { runner.map_request(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM,
        ExecutionSession{nullptr, {}, {}, {}, ""}, original); }, "Foreign"), "ForeignSessionRejects");
    auto changed = runtime_document();
    changed["merge"]["operations"][3]["request"]["hooks"] = changed["merge"]["operations"][0]["request"]["hooks"];
    ServerFixture mapped_stream(upstream.port, changed);
    mapped_stream.server->forward_streaming_request("/v1/chat/completions", raw, sink, true, 1);
    const auto streamed_request = Json::parse(upstream.stream_body);
    check(streamed_request.contains("max_tokens") && streamed_request.at("max_tokens") == 2 && upstream.stream_body != raw, "StreamingRequestChainUsesBoundCell");
    check(throws([&] { mapped_stream.server->forward_streaming_request("/v1/chat/completions", "bad-json", sink); }, "Invalid JSON"), "MappedStreamingRejectsInvalidJson");
    auto hook_free = runtime_document();
    hook_free["merge"].erase("hooks");
    hook_free["merge"]["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
    hook_free["merge"]["launch"]["argv"].erase(1);
    hook_free["merge"]["launch"]["backend"] = {{"option", "engine"}, {"fallback", "first_supported"}, {"on_empty", {{"operation", "fake"}, {"target", "cpu"}}}};
    for (auto& op : hook_free["merge"]["operations"]) { op["request"].erase("hooks"); op.erase("response"); }
    hook_free["merge"]["operations"][0]["request"]["set"]["/from_frame"] = {{"ref", "/request/marker"}};
    ServerFixture external(upstream.port, hook_free, true);
    const auto response = external.server->chat_completion({{"marker", 23}});
    check(response["from_frame"] == 23 && response["model"] == "upstream-model", "HookFreeExternalRunsThroughConfiguredServerAndFrame");
    const auto external_executable = runtime_fixture::executable(hook_free, true);
    const auto external_entry = ConfiguredBackendEntry::make(external_executable);
    RecipeOptions fallback; fallback.set_option("fork_engine", "");
    DefinitionRunner(external_executable).prepare_load({"fallback", info, fallback, "error", {}, {}}, external_entry->spec, launch);
    check(launch.events.back() == "port", "ExplicitEmptyBackendUsesCapturedFallback");
    auto custom_document = runtime_document();
    custom_document["merge"]["launch"]["prepare_order"] = {"backend", "custom_args", "hook", "artifacts", "program", "ports"};
    const auto custom_executable = runtime_fixture::executable(custom_document);
    const auto custom_entry = ConfiguredBackendEntry::make(custom_executable);
    RecipeOptions custom_options; custom_options.set_option("fork_args", "-d=foreign");
    launch.events.clear();
    check(throws([&] { DefinitionRunner(custom_executable).prepare_load({"custom", info, custom_options, "error", {}, {}}, custom_entry->spec, launch); }, "reserved") &&
        std::find(launch.events.begin(), launch.events.end(), "program") == launch.events.end(), "PrepareOrderRejectsReservedAliasBeforeProgram");
    launch.events.clear();
    check(throws([&] { runner.prepare_load({"foreign", info, options, "error", {}, {}}, external_entry->spec, launch); }, "ownership") && launch.events.empty(), "LeafSpecOwnershipBeforeEffects");
    } catch (const std::exception& error) {
        std::printf("Runtime rejection: %s\n", error.what());
        check(false, "RunnerEndToEndDispatchAndFrame");
    }
    return finish();
}
