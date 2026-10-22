#pragma once

#include "backend_runtime_fixture.h"
#include "lemon/system_info.h"

#include <array>
#include <fstream>

namespace runtime_fixture {

inline void llamacpp_install_parity(const std::shared_ptr<const ExecutableDefinition>& executable,
    const ConfiguredBackendEntry& entry) {
    const auto load = [](const std::string& name) {
        std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/llamacpp-oracle/" + name + ".json");
        return Json::parse(input);
    };
    const auto cases = load("cases"), pins = load("pins");
    const DefinitionRunner runner(executable);
    unsigned matched = 0;
    for (const auto& view : std::array<std::string, 4>{"linux_x64", "linux_arm64", "windows_x64", "macos_arm64"}) {
        const auto expected = load(view);
        const std::string os = view.rfind("windows", 0) == 0 ? "windows" : view.rfind("macos", 0) == 0 ? "macos" : "linux";
        const std::string machine = view.find("arm64") == std::string::npos ? "x86_64" : "aarch64";
        unsigned count = 0;
        for (const auto& input : cases) {
            if (input.at("kind") != "install") continue;
            ++count;
            Json events = Json::array(), actual = Json::object();
            const auto facts = input.value("facts", Json::object());
            const auto unsupported = [&](const std::string& backend) -> std::string {
                events.push_back("unsupported:" + backend);
                throw std::runtime_error("injected unsupported backend: " + backend);
            };
            rt::InstallFactSources sources{
                [&] {
                    events.push_back("versions:resources/backend_versions.json");
                    const auto config = facts.value("versions", pins);
                    if (!config.contains("therock") || !config["therock"].is_object() || !config["therock"].contains("version") || !config["therock"]["version"].is_string())
                        throw std::runtime_error("backend_versions.json is missing 'therock.version'");
                    return config["therock"]["version"].get<std::string>();
                },
                [&] {
                    events.push_back("rocm-arch");
                    const auto family = SystemInfo::rocm_asset_family(facts.value("rocm_arch", "gfx1151"));
                    return family.empty() ? unsupported("rocm-nightly") : family;
                },
                [&] {
                    events.push_back("cuda-arch");
                    const auto architecture = facts.value("cuda_arch", "sm_120");
                    return architecture.empty() ? unsupported("cuda") : architecture;
                },
                [&] { events.push_back("runtime-config"); return facts.value("no_config", false) ? "stable" : facts.value("channel", "stable"); }
            };
            const auto phase = input.at("phase") == "repository" ? InstallPhase::Repository : InstallPhase::Asset;
            InstallContext context{entry.spec, {}, "llamacpp", "llamacpp", input.at("backend"), "", "b1111", "b2222", {os, machine, ""}, phase, executable};
            try {
                const auto params = runner.install_params(context, sources);
                actual["params"] = {{"repo", params.repo}, {"filename", params.filename}, {"version_override", params.version_override}, {"sha256", params.sha256}};
                actual["spec"] = {{"recipe", entry.spec.recipe}, {"binary", entry.spec.binary}, {"pin_recipe", entry.spec.pin_recipe},
                    {"public_recipe", entry.spec.public_recipe}, {"split_archive", entry.spec.supports_split_archive}};
                actual["error"] = "";
            } catch (const std::invalid_argument& error) {
                actual["error"] = error.what(); actual["exception"] = "invalid_argument";
            } catch (const std::exception& error) {
                actual["error"] = error.what(); actual["exception"] = "exception";
            }
            actual["events"] = events;
            actual["command"] = nullptr;
            const auto id = input.at("id").get<std::string>();
            const auto difference = Json::diff(expected.at("cases").at(id), actual);
            check(difference.empty(), "FrozenInstallParity:" + view + ":" + id);
            if (!difference.empty()) std::cout << difference.dump() << '\n';
            else ++matched;
        }
        check(count == 66, "FrozenInstallCaseCount:" + view);
    }
    check(matched == 264, "FrozenInstallParityAll264Cases");
}

inline void llamacpp_runtime_parity(const std::shared_ptr<const ExecutableDefinition>& executable) {
    const auto load = [](const std::string& name) {
        std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/llamacpp-oracle/" + name + ".json");
        return Json::parse(input);
    };
    const auto cases = load("cases");
    const DefinitionRunner runner(executable);
    unsigned matched = 0;
    for (const auto& view : std::array<std::string, 4>{"linux_x64", "linux_arm64", "windows_x64", "macos_arm64"}) {
        const auto expected = load(view);
        unsigned count = 0;
        for (const auto& input : cases) {
            if (input.at("kind") != "runtime") continue;
            ++count;
            RecipeOptions options("llamacpp", {{"ctx_size", 4096}, {"llamacpp_backend", view == "macos_arm64" ? "metal" : "cpu"}});
            options.set_option("llamacpp_device", ""); options.set_option("llamacpp_args", "");
            const auto option_values = input.value("options", Json::object());
            for (const auto& [key, value] : option_values.items()) options.set_option(key, value);
            const auto model_input = input.value("model", Json::object());
            ModelInfo info;
            info.labels = model_input.value("labels", std::vector<std::string>{});
            info.checkpoints["draft"] = model_input.value("draft_checkpoint", "");
            Json actual = {{"requested_options", options.to_json()}, {"events", Json::array()}, {"command", nullptr}};
            try {
                runner.resolve_runtime_options(info, options);
                actual["resolved_options"] = options.to_json(); actual["error"] = "";
            } catch (const std::invalid_argument& error) {
                actual["error"] = error.what(); actual["exception"] = "invalid_argument";
            } catch (const std::exception& error) {
                actual["error"] = error.what(); actual["exception"] = "exception";
            }
            const auto id = input.at("id").get<std::string>();
            const auto difference = Json::diff(expected.at("cases").at(id), actual);
            check(difference.empty(), "FrozenRuntimeParity:" + view + ":" + id);
            if (!difference.empty()) std::cout << difference.dump() << '\n';
            else ++matched;
        }
        check(count == 29, "FrozenRuntimeCaseCount:" + view);
    }
    check(matched == 116, "FrozenRuntimeParityAll116Cases");
}

inline void llamacpp_protocol_parity(const std::shared_ptr<const ExecutableDefinition>& executable,
    const std::shared_ptr<const ConfiguredBackendEntry>& entry) {
    const DefinitionRunner runner(executable);
    ConfiguredServer server(entry, BackendContext{"error"});
    RecipeOptions options("llamacpp", {});
    server.set_model_metadata("loaded-model", "fixture/repo", ModelType::LLM, DEVICE_NPU, options);
    const ExecutionSession session{executable, {}, {}, {}, "loaded-model"};
    unsigned buffered = 0, streamed = 0, raw = 0;
    for (const auto& view : std::array<std::string, 4>{"linux_x64", "linux_arm64", "windows_x64", "macos_arm64"}) {
        std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/llamacpp-oracle/" + view + ".json");
        const auto expected = Json::parse(input).at("protocol");
        Json bindings = Json::array(), matrix = Json::array();
        for (const auto& binding : entry->bindings) {
            const auto& description = binding.description;
            bindings.push_back({{"operation", static_cast<int>(description.operation)}, {"delivery", static_cast<int>(description.delivery)},
                {"models", description.model_types}, {"provenance", static_cast<int>(description.provenance)}});
        }
        for (std::size_t op = 0; op < k_backend_operation_count; ++op)
            for (std::size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery)
                for (const auto mode : k_operation_model_types)
                    matrix.push_back(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode));
        check(bindings == expected.at("bindings") && bindings.size() == 11 && matrix == expected.at("matrix"), "FrozenProtocolElevenCells:" + view);
        for (const auto& [key, transcript] : expected.at("buffered").items()) {
            entries.clear();
            Json events = Json::array(), forwarded;
            const auto flush = [&] {
                for (const auto& hook : entries) {
                    if (hook == "max_tokens_alias") events.push_back("alias");
                    else if (hook == "tool_schema_limits") events.push_back("sanitizer");
                    else if (hook == "client_model") events.push_back("normalization");
                    else throw std::logic_error("Unexpected protocol hook: " + hook);
                }
                entries.clear();
            };
            const auto separator = key.find(':');
            const auto operation = static_cast<BackendOperation>(std::stoi(key.substr(0, separator)));
            const auto request = transcript.at("input");
            const Json reply = key.substr(separator + 1) == "3" ? Json::array({1}) : Json{{"model", "backend-model"}, {"witness", true}};
            BackendTransport transport({
                [&](const auto& path, const auto& body, long) { flush(); events.push_back("POST:" + path); forwarded = {{"method", "POST"}, {"path", path}, {"body", body}}; return reply; },
                [&](const auto& path, long) { flush(); events.push_back("GET:" + path); forwarded = {{"method", "GET"}, {"path", path}}; return reply; }, {}});
            auto mapped = runner.map_request(operation, OperationDelivery::Buffered, ModelType::LLM, session, request);
            if (operation == BackendOperation::SlotAction) {
                const auto id = mapped.path.find("{slot_id}"); mapped.path.replace(id, 9, "2");
                const auto action = mapped.path.find("{action}"); mapped.path.replace(action, 8, "save");
            }
            const auto response = runner.map_response(operation, ModelType::LLM, session, request, transport.send_json(mapped));
            flush();
            const Json actual = {{"input", request}, {"forwarded", forwarded}, {"response", response}, {"events", events}};
            check(actual == transcript, "FrozenBufferedTranscript:" + view + ":" + key);
            if (actual != transcript) std::cout << Json::diff(transcript, actual).dump() << '\n';
            ++buffered;
        }
        for (const auto& [key, transcript] : expected.at("streaming").items()) {
            entries.clear();
            Json events = Json::array(), forwarded;
            std::string bytes;
            const auto separator = key.find(':');
            const auto path = key.substr(0, separator);
            const bool sse = key.substr(key.rfind(':') + 1) == "sse";
            const std::string body = transcript.at("input");
            BackendTransport transport({{}, {}, [&](const auto& endpoint, const auto& sent, auto& sink, bool framed, long timeout, WrappedServer::TelemetryCallback telemetry) {
                for (const auto& hook : entries) {
                    if (hook == "max_tokens_alias") events.push_back("alias");
                    else if (hook == "tool_schema_limits") events.push_back("sanitizer");
                    else throw std::logic_error("Unexpected streaming hook: " + hook);
                }
                events.push_back("stream:" + endpoint);
                forwarded = {{"method", "POST"}, {"path", endpoint}, {"body", sent}, {"sse", framed}, {"timeout", timeout}, {"telemetry", static_cast<bool>(telemetry)}};
                const std::string output = "data: {\"model\":\"backend-model\"}\n\ndata: [DONE]\n\n";
                sink.write(output.data(), output.size());
            }});
            httplib::DataSink sink;
            sink.write = [&](const char* data, std::size_t size) { bytes.append(data, size); return true; };
            sink.is_writable = [] { return true; }; sink.done = [] {};
            const auto mapped = runner.map_stream(path, ModelType::LLM, session, body, sse, sse ? 0 : 13);
            transport.send_stream(mapped, sink, sse ? WrappedServer::TelemetryCallback([](const auto&) {}) : nullptr);
            const Json actual = {{"input", body}, {"forwarded", forwarded}, {"bytes", bytes}, {"events", events}};
            check(actual == transcript, "FrozenStreamingTranscript:" + view + ":" + key);
            if (actual != transcript) std::cout << Json::diff(transcript, actual).dump() << '\n';
            if (path == "/v1/completions") {
                check(mapped.original_body == body && forwarded.at("body") == body && events == Json::array({"stream:" + path}),
                    "CompletionStreamingPreservesEveryOriginalByte:" + view + ":" + key);
                ++raw;
            }
            ++streamed;
        }
    }
    check(buffered == 128 && streamed == 160 && raw == 40, "FrozenProtocolTranscriptCountsExact");
    const auto mapped = runner.map_request(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM, session, {{"model", "client"}});
    const auto error = runner.map_response(BackendOperation::ChatCompletion, ModelType::LLM, session, {{"model", "client"}}, {{"model", "backend"}, {"error", "witness"}});
    check(error.at("model") == "client" && error.at("error") == "witness", "DeclaredResponseNormalizationConsumesErrorObjects");
}

inline void llamacpp_loopback(const std::shared_ptr<const ConfiguredBackendEntry>& entry) {
    httplib::Server upstream;
    std::string received;
    std::mutex received_mutex;
    const std::string reply = "data: {\"ok\":true}\n\ndata: [DONE]\n\n";
    upstream.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
    upstream.Post("/v1/completions", [&](const auto& request, auto& response) {
        { std::lock_guard<std::mutex> lock(received_mutex); received = request.body; }
        response.set_content(reply, "text/event-stream");
    });
    const int port = upstream.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("Loopback bind failed");
    auto worker = std::async(std::launch::async, [&] { upstream.listen_after_bind(); });
    upstream.wait_until_ready();
    try {
        auto launch = std::make_shared<Launch>();
        launch->port = port; launch->allow_install = true;
        ServerCommand command;
        ConfiguredServer server(entry, BackendContext{"error"}, ConfiguredServerServices{launch,
            [&](auto, const auto&) { return std::make_unique<Process>(command); }});
        ModelInfo info;
        info.model_name = "fake"; info.checkpoints["main"] = "fake"; info.resolved_paths["main"] = "fake.gguf";
        RecipeOptions options("llamacpp", {{"llamacpp_backend", "cpu"}, {"ctx_size", 4096}});
        options.set_option("llamacpp_device", ""); options.set_option("llamacpp_args", "");
        server.set_model_metadata("fake", "fake", ModelType::LLM, DEVICE_CPU, options);
        server.load("fake", info, options);
        server.set_state(ModelState::READY);
        for (const std::string body : {"{ \"model\" : \"fake\", \"stream\" : true }", "{broken", "[1]", "null"}) {
            std::string output;
            httplib::DataSink sink;
            sink.write = [&](const char* bytes, std::size_t size) { output.append(bytes, size); return true; };
            sink.is_writable = [] { return true; }; sink.done = [] {};
            for (const bool sse : {false, true}) {
                output.clear();
                server.forward_streaming_request("/v1/completions", body, sink, sse, 5);
                std::lock_guard<std::mutex> lock(received_mutex);
                check(received == body && output == reply, "CompletionStreamingRawBytesThroughConfiguredServerAndLoopback");
            }
        }
        server.unload();
    } catch (...) { upstream.stop(); worker.get(); throw; }
    upstream.stop(); worker.get();
}

}
