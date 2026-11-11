#pragma once

Json production_gufo_document() {
    const auto candidate = std::getenv("LEMON_GUFO_PARITY_CANDIDATE");
    const auto file = candidate ? fs::path(candidate) : fs::path(LEMON_TEST_FIXTURES).parent_path().parent_path() / "src/cpp/resources/backends/gufo.json";
    std::ifstream input(file);
    return Json::parse(input);
}

Fixture production_gufo_fixture(const Json& doc) {
    const auto& owner = reviewed_gufo_execution_owner();
    const auto result = admit({}, builtins(false, &doc, false, &owner));
    if (!result.catalog) {
        diagnostics(result.diagnostics);
        check(false, "ProductionGufoFixedExecutionOwner");
        throw std::logic_error("Production Gufo admission failed");
    }
    const auto token = result.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1});
    const auto compiled = compile_execution_definition(token);
    if (!compiled.definition) { diagnostics(compiled.diagnostics); throw std::logic_error("Production Gufo compilation failed"); }
    return {result.catalog, token, compiled.definition};
}

void production_gufo_protocols(const fs::path& scratch, const Fixture& fixture, const Json& doc) {
    const auto bound = bind(fixture);
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    const auto& owner = reviewed_gufo_execution_owner();
    const std::vector<BuiltinRegistrationSeed> seeds{{&entry->descriptor, "gufo", {}}};
    const auto parents = CatalogAdmissionBuilder::test_with_seeds(builtins(false, &doc, false, &owner), seeds);
    const auto catalog = build_backend_catalog(parents, {});
    check(catalog.catalog && catalog.catalog->registrations().size() == 1, "ProductionGufoIsolatedCatalogFactory");
    if (!catalog.catalog) { diagnostics(catalog.diagnostics); return; }
    FakeHost host;
    auto containers = std::make_shared<utils::ContainerManager>(host.host());
    BackendManager manager(containers);
    BackendManagerTestAccess::pins(manager, Json{{"gufo", {{"rocm", "rocm-10.0-gufo@sha256:2d7e9995ff2dfd7035f610a419b4f38d5d1604c0c7ab7f3b81415ad6f3e8db8b"}}}});
    auto services = std::make_shared<Services>(manager);
    httplib::Server http;
    Json observed = Json::array();
    http.Get("/ready", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
    http.Post(R"(/v1/(chat/completions|completions|responses))", [&](const auto& request, auto& response) {
        const auto body = Json::parse(request.body);
        observed.push_back({{"path", request.path}, {"body", body}});
        if (body.value("stream", false)) response.set_content("data: {\"id\":\"production-gufo\"}\n\ndata: [DONE]\n\n", "text/event-stream");
        else response.set_content(Json{{"id", "production-gufo"}, {"model", "backend-name"}}.dump(), "application/json");
    });
    services->port = http.bind_to_any_port("127.0.0.1");
    if (services->port <= 0) throw std::runtime_error("Cannot bind production loopback");
    std::thread listener([&] { http.listen_after_bind(); });
    struct Cleanup { httplib::Server& server; std::thread& thread; ~Cleanup() { server.stop(); if (thread.joinable()) thread.join(); } } cleanup{http, listener};
    http.wait_until_ready();
    ClientState client;
    unsigned native = 0;
    ConfiguredServerServices effects;
    effects.launch = services; effects.containers = containers;
    effects.make_process = [&](auto, const auto&) -> std::unique_ptr<ServerProcess> { ++native; throw std::logic_error("Native fallback forbidden"); };
    effects.make_container_client = [&](auto) { return std::make_unique<FakeClient>(client); };
    auto instance = catalog.catalog->registrations()[0].create({"error", nullptr, &manager});
    auto* server = dynamic_cast<ConfiguredServer*>(instance.get());
    check(server != nullptr, "ProductionGufoFactoryCreatesConfiguredServer");
    if (!server) return;
    check(!server->supports_operation(BackendOperation::Embeddings, OperationDelivery::Buffered, ModelType::LLM) &&
        !server->supports_operation(BackendOperation::Reranking, OperationDelivery::Buffered, ModelType::LLM), "ProductionGufoNoOptionalCapabilityAdapters");
    ConfiguredServerTestAccess::effects(*server, std::move(effects));
    ModelInfo model; model.recipe = "gufo"; model.model_name = "production-http";
    model.resolved_paths["main"] = (scratch / "production-inventory/main/checkpoint.bin").string();
    RecipeOptions options;
    server->load(model.model_name, model, options);
    for (const auto& path : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
        Json request{{"model", "caller"}, {"max_tokens", 2}, {"extra", "unchanged"}};
        Json response;
        if (std::string(path) == "/v1/chat/completions") response = server->chat_completion(request);
        else if (std::string(path) == "/v1/completions") response = server->completion(request);
        else response = server->responses(request);
        check(observed.back() == Json{{"path", path}, {"body", request}} && response == Json{{"id", "production-gufo"}, {"model", "backend-name"}}, "ProductionGufoBufferedIdentity:" + std::string(path));
        request["stream"] = true;
        std::string bytes;
        httplib::DataSink sink;
        sink.write = [&](const char* data, std::size_t size) { bytes.append(data, size); return true; };
        sink.is_writable = [] { return true; };
        sink.done = [] {};
        server->forward_streaming_request(path, request.dump(), sink, true, 2);
        check(observed.back() == Json{{"path", path}, {"body", request}} && bytes == "data: {\"id\":\"production-gufo\"}\n\ndata: [DONE]\n\n", "ProductionGufoStreamingIdentity:" + std::string(path));
    }
    server->unload();
    check(native == 0 && client.starts == 1 && client.stops == 1 && !client.alive, "ProductionGufoProtocolContainerTeardown");
    std::ofstream(scratch / "production-protocols.json") << observed.dump(2) << '\n';
}

void production_gufo_parity(const fs::path& scratch) {
    const auto expected = Json::parse(read_bytes("gufo.production-parity.json"));
    const auto doc = production_gufo_document();
    const auto fixture = production_gufo_fixture(doc);
    const auto bound = bind(fixture);
    check(bound.definition != nullptr, "ProductionGufoBindsWithReviewedGrant");
    if (!bound.definition) { diagnostics(bound.diagnostics); return; }
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    const auto& descriptor = entry->descriptor;
    check(descriptor.default_device == DEVICE_GPU && !descriptor.selectable_backend && descriptor.uses_ctx_size &&
        !descriptor.dynamic_models && descriptor.exposes_prometheus_metrics && descriptor.rocm_requires_cwsr_fix &&
        descriptor.version_policy == VersionPolicy::Exact && descriptor.support.size() == 1 &&
        descriptor.support[0].tier == BackendTier::Guest && descriptor.support[0].format == BackendFormat::Container &&
        descriptor.support[0].supported_os == std::set<std::string>{"linux"} &&
        descriptor.support[0].devices == DeviceConstraints{{"amd_gpu", {"gfx1151"}}},
        "ProductionGufoExactSupportAndMetadata");
    check(descriptor.default_capabilities.empty() && descriptor.supported_modes == std::vector<std::string>{"chat"} &&
        k_gufo_parity_capabilities[0].mask == expected.at("capability_mask").get<uint32_t>() &&
        k_gufo_parity_capabilities[0].mask == 0 &&
        entry->bindings.size() == 6, "ProductionGufoCapabilityMaskDoesNotOverclaim");
    Json protocols = Json::array();
    for (const auto& op : fixture.compiled->normalized_declaration().at("operations"))
        protocols.push_back({{"operation", op.at("operation")}, {"delivery", op.at("delivery")},
            {"path", op.at("transport").at("path")}, {"request", op.at("request").at("base")}});
    check(protocols == expected.at("protocols"), "ProductionGufoFrozenProtocolSurface");
    const auto stripped = AuthorityInspection::test_without_grants(*fixture.token);
    check(AuthorityInspection::grant_count(*fixture.token) == 3 && AuthorityInspection::grant_count(*stripped) == 0 &&
        AuthorityInspection::permits_execution(*stripped, fixture.compiled->normalized_declaration()), "ProductionGufoOwnerSurvivesHookRemoval");
    const auto& owner = reviewed_gufo_execution_owner();
    check(owner.digest == expected.at("owner_digest") &&
        owner.digest == v2::identity("lemonade.backend-container-owner.v1\n", owner.semantics), "ProductionGufoFixedOwnerDigest");
    auto foreign = fixture.compiled->normalized_declaration();
    foreign["containers"]["rocm"]["repository"] = "docker.io/kyuz0/different";
    check(!AuthorityInspection::permits_execution(*stripped, foreign), "ProductionGufoGrantCannotAdmitForeignRepository");
    const auto production = freeze_builtin_catalog();
    const auto active = admit({}, production);
    check(active.catalog && !active.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1}) &&
        embedded_v2_policies().leaves.size() == 2 && embedded_v2_policies().execution_owners.empty(), "ProductionGufoLeafDeliberatelyInactive");
    const auto root = fs::path(LEMON_TEST_FIXTURES).parent_path().parent_path();
    std::ifstream pin_input(root / "src/cpp/resources/backend_versions.json");
    const auto pins = Json::parse(pin_input);
    check(pins.at("gufo").at("rocm") == expected.at("pin"), "ProductionGufoStackPinCopiedExactly");
    std::ifstream model_input(root / "src/cpp/resources/server_models.json");
    const auto registry = Json::parse(model_input);
    for (const auto& [name, model] : expected.at("models").items()) check(registry.at(name) == model, "ProductionGufoExactRegistry:" + name);
    FakeHost host;
    auto manager = std::make_shared<utils::ContainerManager>(host.host());
    BackendManager backend_manager(manager);
    BackendManagerTestAccess::pins(backend_manager, pins);
    const auto policy = descriptor.containers.at("rocm");
    rejects([&] { effective_container_pin(policy, Json{{"gufo", {{"rocm", "latest"}}}}, ""); }, "ProductionGufoMutableTagRejected");
    Services services(backend_manager);
    services.install = false;
    ModelInfo model; model.recipe = "gufo"; model.model_name = "production-fixture";
    for (const auto& role : {"main", "mmproj", "draft"}) {
        const auto file = scratch / "production-inventory" / role / "checkpoint.bin";
        fs::create_directories(file.parent_path()); std::ofstream(file) << "CPU fixture only";
        model.resolved_paths[role] = file.string();
    }
    RecipeOptions options; options.set_option("ctx_size", 8192);
    Json transcripts = Json::array();
    DefinitionRunner runner(bound.definition);
    for (const auto& [mode, flag] : expected.at("speculative").items()) {
        model.extras["gufo_speculative"] = mode;
        const auto prepared = runner.prepare_load({"production-fixture", model, options, "error"}, entry->spec, services);
        const auto& plan = prepared.plan;
        auto argv = expected.at("argv").get<std::vector<std::string>>();
        for (auto& value : argv) { if (value == "$mode") value = mode; if (value == "$draft_flag") value = flag; }
        check(plan.command.args == argv, "ProductionGufoFrozenArgv:" + mode);
        Json destinations = Json::array();
        for (const auto& mount : plan.container->mounts) destinations.push_back(mount.destination);
        check(destinations == expected.at("mounts"), "ProductionGufoFrozenMountDestinations");
        check(plan.container->devices == expected.at("devices").get<std::vector<std::string>>() && plan.container->cap_add.empty(), "ProductionGufoFrozenDevices");
        check(plan.container->image == expected.at("repository").get<std::string>() + "@sha256:" +
            expected.at("pin").get<std::string>().substr(expected.at("pin").get<std::string>().find("@sha256:") + 8), "ProductionGufoFrozenImageIdentity");
        check(plan.command.ready_endpoint == expected.at("readiness").at("path") && plan.readiness_timeout_seconds == 2 &&
            plan.output.filter_health_logs, "ProductionGufoFrozenReadiness");
        check(plan.command.env == std::vector<std::pair<std::string, std::string>>{{"HIP_VISIBLE_DEVICES", "1"}}, "ProductionGufoTrustedGpuEnvironment");
        ClientState client;
        ContainerProcess process({}, *plan.container, manager, std::make_unique<FakeClient>(client));
        process.start(plan.command); process.stop();
        check(std::find(client.invocation.args.begin(), client.invocation.args.end(), "HOME=/tmp") != client.invocation.args.end() &&
            client.starts == 1 && client.stops == 1, "ProductionGufoFakeContainerLifecycle");
        transcripts.push_back({{"mode", mode}, {"argv", plan.command.args}, {"runtime_argv", client.invocation.args}, {"mounts", destinations}});
    }
    model.extras["gufo_speculative"] = "invalid";
    bool invalid = false;
    try { runner.prepare_load({"production-fixture", model, options, "error"}, entry->spec, services); }
    catch (const std::invalid_argument& error) { invalid = std::string(error.what()) == "gufo: a draft model needs gufo_speculative set to dflash2, mtp or dspark, not 'invalid'"; }
    check(invalid, "ProductionGufoExactInvalidModeError");
    model.resolved_paths["mmproj"] = ""; model.resolved_paths["draft"] = "";
    const auto minimal = runner.prepare_load({"production-fixture", model, options, "error"}, entry->spec, services);
    check(minimal.plan.command.args.size() == 12 && minimal.plan.container->mounts.size() == 1, "ProductionGufoOptionalArtifactsAndUnusedMode");
    for (const auto& flag : doc.at("merge").at("launch").at("custom_args").at("reserved")) {
        RecipeOptions bad = options; bad.set_option("gufo_args", flag.get<std::string>() + "=forbidden");
        rejects([&] { runner.prepare_load({"production-fixture", model, bad, "error"}, entry->spec, services); }, "ProductionGufoReservedCustomFlag:" + flag.get<std::string>());
    }
    RecipeOptions custom = options; custom.set_option("gufo_args", "--batch-size 7");
    const auto custom_plan = runner.prepare_load({"production-fixture", model, custom, "error"}, entry->spec, services);
    check(custom_plan.plan.command.args.back() == "7", "ProductionGufoCustomArgsAppended");
    allow_entries = true;
    check(entry->backend_ops()->validate_registration_checkpoint("org/repo:model.gguf") == "" &&
        !entry->backend_ops()->validate_registration_checkpoint("org/Model-GGUF").empty(), "ProductionGufoInheritsLlamaGgufValidation");
    allow_entries = false; calls.clear();
    std::ofstream(scratch / "production-transcript.json") << transcripts.dump(2) << '\n';
    if (protocols == expected.at("protocols")) production_gufo_protocols(scratch, fixture, doc);
}
