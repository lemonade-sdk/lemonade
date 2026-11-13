#pragma once

namespace {
struct Ds4Services final : HostServices {
    IntegratedMemoryPool pool{true, 8.0, 62.0};
    FreeMemoryFact free{true, 45.0};
    std::optional<std::uint64_t> main_bytes{60ULL * 1073741824ULL}, draft_bytes{4ULL * 1073741824ULL};
    bool stat_known = true;
    mutable unsigned pool_queries = 0, free_queries = 0, size_queries = 0;
    IntegratedMemoryPool integrated_memory_pool() const override { ++pool_queries; return pool; }
    FreeMemoryFact free_gpu_memory() const override { ++free_queries; return free; }
    std::optional<ArtifactByteFact> artifact_bytes(const std::string& path) const override {
        ++size_queries;
        const auto value = fs::path(path).filename() == "main.fake" ? main_bytes : draft_bytes;
        return value ? std::optional<ArtifactByteFact>(ArtifactByteFact{stat_known, *value}) : std::nullopt;
    }
};
class BodyEcho {
public:
    BodyEcho() {
        server_.Post("/v1/.*", [](const httplib::Request& request, httplib::Response& response) { response.set_content(request.body, "application/json"); });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error("Cannot bind body echo");
        worker_ = std::thread([this] { server_.listen_after_bind(); });
    }
    ~BodyEcho() { server_.stop(); if (worker_.joinable()) worker_.join(); }
    std::string post(const std::string& path, const std::string& body) const {
        httplib::Client client("127.0.0.1", port_);
        client.set_connection_timeout(2, 0); client.set_read_timeout(2, 0);
        const auto response = client.Post(path, body, "application/json");
        if (!response || response->status != 200) throw std::runtime_error("Body echo failed");
        return response->body;
    }
private:
    httplib::Server server_;
    std::thread worker_;
    int port_ = 0;
};
void ds4_contract(const fs::path& scratch) {
    const auto fixture = Json::parse(read_bytes("c6b.ds4.json"));
    const auto backend = entry(fixture.at("definition"));
    for (const auto* invalid : {"unknown-transform", "wrong-transform-operation", "unknown-memory-path", "wrong-memory-type", "default-memory-fact", "memory-record-object", "memory-before-backend"}) {
        auto changed = fixture.at("definition"); const std::string name = invalid;
        if (name == "unknown-transform") changed["merge"]["operations"][0]["request"]["content_transform"] = "strip-any-prefix";
        if (name == "wrong-transform-operation") changed["merge"]["operations"][2]["request"]["content_transform"] = "ds4_no_think_v1";
        if (name == "unknown-memory-path") changed["merge"]["launch"]["argv"][0]["value"] = Json{{"ref", "/memory/private"}};
        if (name == "wrong-memory-type") changed["merge"]["launch"]["argv"][0]["value"] = Json{{"ref", "/memory/resident"}, {"type", "string"}};
        if (name == "default-memory-fact") changed["merge"]["launch"]["argv"][0]["value"] = Json{{"ref", "/memory/expert_cache_gb"}, {"default", 0}};
        if (name == "memory-before-backend") std::swap(changed["merge"]["launch"]["prepare_order"][0], changed["merge"]["launch"]["prepare_order"][1]);
        if (name == "memory-record-object") changed["merge"]["launch"]["execution"]["cases"][0]["memory_plan"] = Json{{"kind", "ds4_rocm_v1"}};
        rejects([&] { entry(changed); }, "Ds4ClosedContract:" + name);
    }

    DefinitionRunner runner(backend->executable_definition);
    const auto bundle = scratch / "ds4"; fs::create_directories(bundle);
    for (const auto* name : {"main.fake", "draft.fake"}) std::ofstream(bundle / name) << "fixture";
    ModelInfo model; model.recipe = "gufo"; model.type = ModelType::LLM;
    model.resolved_paths["main"] = (bundle / "main.fake").string();
    RecipeOptions options; options.set_option("ctx_size", 8192);
    Ds4Services services;
    const auto prepare = [&] { return runner.prepare_load({"fixture", model, options, "error", {}, {}}, backend->spec, services); };
    std::shared_ptr<const ExecutionSession> session;
    for (const auto& row : fixture.at("memory_cases")) {
        services.pool = {row.at("available").get<bool>(), row.at("vram").get<double>(), row.at("virtual").get<double>()};
        services.free = {row.at("free_known").get<bool>(), row.at("free_gib").get<double>()};
        services.main_bytes = row.at("main_bytes").get<std::uint64_t>(); services.draft_bytes = row.at("draft_bytes").get<std::uint64_t>();
        services.free_queries = services.pool_queries = services.size_queries = 0;
        model.resolved_paths["draft"] = *services.draft_bytes ? (bundle / "draft.fake").string() : "";
        model.min_resident_gb = row.at("min_resident_gb");
        const auto name = row.at("name").get<std::string>();
        if (!row.at("error").is_null()) { rejects(prepare, "Ds4ExactMemoryRefusal:" + name, row.at("error")); continue; }
        const auto planned = prepare(); session = planned.session;
        const auto& facts = session->frame.values.at("memory");
        const auto& expected = row.at("expected");
        check(facts.at("resident") == expected.at("resident") && facts.at("expert_cache_gb") == expected.at("expert_cache_gb") &&
            facts.at("model_cache_gb") == expected.at("model_cache_gb") && facts.at("free_reserve_gb") == expected.at("free_reserve_gb") &&
            facts.at("weights_gib") == expected.at("weights_gib") && facts.at("pool_gib") == expected.at("pool_gib") &&
            facts.at("main_bytes") == row.at("main_bytes") && facts.at("draft_bytes") == row.at("draft_bytes") &&
            facts.at("integrated_available") == row.at("available") && facts.at("igpu_vram_gib") == row.at("vram") &&
            facts.at("igpu_virtual_gib") == row.at("virtual"), "Ds4SourceMemoryPlan:" + name);
        check(planned.plan.command.program == "ds4-server" && planned.plan.command.args == expected.at("args").get<std::vector<std::string>>() &&
            planned.plan.command.env == expected.at("env").get<std::vector<std::pair<std::string, std::string>>>() &&
            planned.plan.command.ready_endpoint == "/v1/models", "Ds4SourceMemoryArgvAndEnvironment:" + name);
        check(services.pool_queries == 1 && services.free_queries == expected.at("free_queries").get<unsigned>() &&
            services.size_queries == (model.resolved_path("draft").empty() ? 1u : 2u), "Ds4MemoryFactEffects:" + name);
        check(planned.plan.container && planned.plan.container->mounts.size() == (expected.at("resident") == true && *services.draft_bytes ? 2u : 1u), "Ds4ResidencySelectsDraftMount:" + name);
    }
    const auto restore = [&] {
        services.pool = {true, 8.0, 62.0}; services.free = {true, 45.0};
        services.main_bytes = 60ULL * 1073741824ULL; services.draft_bytes = 4ULL * 1073741824ULL;
    };
    restore(); model.resolved_paths["draft"] = (bundle / "draft.fake").string(); model.min_resident_gb = 8;
    for (const auto* missing : {"available", "vram", "virtual", "main", "draft", "free-known", "free-gib"}) {
        const std::string name = missing;
        if (name == "available") services.pool.available.reset();
        if (name == "vram") services.pool.vram_gib.reset();
        if (name == "virtual") services.pool.virtual_gib.reset();
        if (name == "main") services.main_bytes.reset();
        if (name == "draft") services.draft_bytes.reset();
        if (name == "free-known") services.free.known.reset();
        if (name == "free-gib") services.free.gib.reset();
        rejects(prepare, "Ds4MissingMemoryFactRejected:" + name, "Missing DS4 memory fact:"); restore();
    }
    services.stat_known = false; services.main_bytes = 0; model.resolved_paths["draft"] = "";
    model.min_resident_gb = 0;
    const auto failed_stat = prepare();
    check(failed_stat.session->frame.values.at("memory").at("main_bytes_known") == false &&
        failed_stat.session->frame.values.at("memory").at("weights_gib") == 0.0 &&
        failed_stat.session->frame.values.at("memory").at("resident") == false, "Ds4ObservedStatFailureIsExplicitZero");
    services.stat_known = true; restore(); model.resolved_paths["draft"] = (bundle / "draft.fake").string(); model.min_resident_gb = 8;
    session = prepare().session;
    BodyEcho echo;
    Json buffered_wire;
    std::string streaming_wire;
    BackendTransport transport({[&](const auto& path, const Json& body, long) { buffered_wire = Json::parse(echo.post(path, body.dump())); return Json::object(); }, {},
        [&](const auto& path, const auto& body, httplib::DataSink&, bool, long, auto) { streaming_wire = echo.post(path, body); }});
    const auto request = fixture.at("request"), expected = fixture.at("expected");
    transport.send_json(runner.map_request(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM, *session, request));
    check(buffered_wire == expected, "Ds4BufferedRewriteWireBodyAllUserMessages");
    httplib::DataSink sink;
    transport.send_stream(runner.map_stream("/v1/chat/completions", ModelType::LLM, *session, request.dump(2), false, 37), sink);
    check(streaming_wire == expected.dump(), "Ds4StreamingRewriteWireBodyAllUserMessages");
    for (const auto& raw : fixture.at("malformed")) {
        const auto text = raw.get<std::string>(); bool preserved = false;
        try { transport.send_stream(runner.map_stream("/v1/chat/completions", ModelType::LLM, *session, text, false, 37), sink); preserved = streaming_wire == text; }
        catch (const std::exception&) {}
        check(preserved, "Ds4MalformedJsonStreamingPassthrough:" + text);
    }
    for (const auto& raw : fixture.at("unchanged")) {
        transport.send_json(runner.map_request(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM, *session, raw));
        check(buffered_wire == raw, "Ds4UnchangedBuffered:" + raw.dump());
        transport.send_stream(runner.map_stream("/v1/chat/completions", ModelType::LLM, *session, " " + raw.dump() + " ", false, 37), sink);
        check(streaming_wire == raw.dump(), "Ds4ValidStreamingJsonReserialized:" + raw.dump());
    }
    const Json bad_role{{"messages", {{{"role", 42}, {"content", "/no_think\nbad"}}}}};
    rejects([&] { runner.map_request(BackendOperation::ChatCompletion, OperationDelivery::Buffered, ModelType::LLM, *session, bad_role); }, "Ds4MalformedRoleSourceExceptionBuffered");
    rejects([&] { runner.map_stream("/v1/chat/completions", ModelType::LLM, *session, bad_role.dump(), true, 37); }, "Ds4MalformedRoleSourceExceptionStreaming");
    for (const auto* path : {"/v1/completions", "/v1/responses"}) {
        const auto raw = request.dump(2); transport.send_stream(runner.map_stream(path, ModelType::LLM, *session, raw, false, 37), sink);
        check(streaming_wire == raw, std::string("Ds4OtherEndpointsUnchanged:") + path);
    }
    auto changed = backend->executable_definition->compiled().normalized_declaration();
    changed["operations"][0]["request"].erase("content_transform");
    check(!AuthorityInspection::permits_execution(backend->executable_definition->compiled().authorized(), changed), "Ds4ContentTransformIsSealed");
    changed = backend->executable_definition->compiled().normalized_declaration(); changed["launch"]["execution"]["cases"][0].erase("memory_plan");
    check(!AuthorityInspection::permits_execution(backend->executable_definition->compiled().authorized(), changed), "Ds4MemoryPlanIsSealed");
}
}
