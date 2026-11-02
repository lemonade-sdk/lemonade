#pragma once

void c9_production_contracts(const std::string& id, const Json& doc, const Fixture& fixture,
    const std::shared_ptr<const ExecutableDefinition>& bound, Services& services, ModelInfo model, RecipeOptions options) {
    const bool llama = doc.at("recipe") == "llamacpp";
    const auto& frozen = load_json(fs::path(LEMON_TEST_FIXTURES) / "backend-v2" / (id + ".production-parity.json"));
    const auto c8a = Json::parse(read_bytes(frozen.at("c8a_fixture")));
    if (llama) check(doc == c8a.at("definition"), "C9ExactC8aProductionDefinition:" + id);
    const auto entry = ConfiguredBackendEntry::make(bound);
    DefinitionRunner runner(bound);
    services.install_failure = true; services.events.clear();
    if (llama) {
        model.extras["hf_load"] = true;
        const auto error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
        check(error == "Model 'fixture' has llama-server download its own weights, which llamacpp:" + id +
            " cannot do from its container's private network. Load it on a llamacpp backend that runs on the host." &&
            services.events.empty(), "C9RefusalBeforeAnyInstall:" + id);
        for (bool explicit_false : {false, true}) {
            model.extras.erase("hf_load"); if (explicit_false) model.extras["hf_load"] = false;
            services.events.clear();
            check(c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); }) ==
                "Fake install failure" && services.events == std::vector<std::string>{"install"}, "C9NonHfInstalls:" + id);
        }
        auto changed = fixture.compiled->normalized_declaration(); changed["launch"].erase("pre_install_refusal");
        check(!AuthorityInspection::permits_execution(*fixture.token, changed), "C9RefusalOwnerSealed:" + id);
    }
    services.install_failure = false;
    const auto scratch = fs::path(LEMON_C9_SCRATCH) / id;
    for (const auto* role : {"main", "mmproj", "draft"}) {
        fs::create_directories(scratch / role);
        const auto path = scratch / role / "checkpoint.fake";
        std::ofstream(path) << "fake"; model.resolved_paths[role] = path.string();
    }
    model.checkpoints["draft"] = "fixture/draft.fake";
    for (const auto* label : {"none", "dflash", "mtp"}) {
        model.labels = std::string(label) == "none" ? std::vector<std::string>{} : std::vector<std::string>{label};
        auto resolved = options; runner.resolve_runtime_options(model, resolved);
        const auto prepared = runner.prepare_load({"fixture", model, resolved, "error"}, entry->spec, services);
        std::vector<std::string> destinations;
        for (const auto& mount : prepared.plan.container->mounts) destinations.push_back(mount.destination);
        check(destinations == std::vector<std::string>{"/mnt/models/checkpoint.fake", "/mnt/models/2/checkpoint.fake", "/mnt/models/3/checkpoint.fake"},
            "C9NumberedCollisionMounts:" + id + ":" + label);
        check(prepared.plan.output.filter_health_logs && prepared.plan.command.ready_endpoint == "/health" && prepared.plan.readiness_timeout_seconds == 2,
            "C9OutputAndReadiness:" + id);
    }
    for (const auto& flag : {"--model", "--port", "--host", "--ctx-size", "--device", "--mmproj", "--model-draft"}) {
        auto invalid = options; invalid.set_option("llamacpp_args", std::string(flag) + "=forbidden");
        check(!c8a_error([&] { runner.prepare_load({"fixture", model, invalid, "error"}, entry->spec, services); }).empty(),
            "C9ReservedArgument:" + id + ":" + flag);
    }
}

void c9_selector(const std::string& id) {
    const auto root = fs::path(LEMON_TEST_FIXTURES).parent_path().parent_path();
    const auto doc = load_json(root / "src/cpp/resources/backends" / (id + ".json"));
    const auto fixture = fixed_fixture(doc); const auto bound = bind(fixture);
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    BackendDescriptor primary = entry->descriptor;
    primary.support.clear();
    unsigned native_calls = 0;
    const BackendRegistration native{&primary, [&](const auto&) -> std::unique_ptr<WrappedServer> { ++native_calls; throw std::runtime_error("CPU primary factory control"); }, nullptr, default_backend_ops()};
    const auto& owner = reviewed_container_execution_owner(id);
    const std::vector<BuiltinRegistrationSeed> seeds{{&primary, "", native}, {&entry->descriptor, id, {}, id}};
    const auto builtins = CatalogAdmissionBuilder::test_with_seeds(binding_fixture::builtins(false, &doc, false, &owner), seeds);
    const auto catalog = build_backend_catalog(builtins, {});
    check(catalog.catalog && catalog.catalog->registrations().size() == 2 && catalog.catalog->data()->descriptors.size() == 1 &&
        catalog.catalog->export_schema().at("recipes").size() == 1 && catalog.catalog->export_schema().at("recipes")[0].at("recipe") == "llamacpp",
        "C9SelectorSharesPublicRecipe:" + id);
    if (!catalog.catalog) { diagnostics(catalog.diagnostics); return; }
    for (const auto* backend : {"", "auto", "cpu", "vulkan", "rocm-stable"}) {
        const auto* selected = select_backend_registration(catalog.catalog->registrations(), "llamacpp", backend);
        check(selected && selected->backend_selector.empty(), "C9NativeSelectorUnchanged:" + id + ":" + backend);
    }
    const auto* selected = select_backend_registration(catalog.catalog->registrations(), "llamacpp", id);
    check(selected && selected->configured_entry && selected->configured_entry->executable_definition->compiled().authorized().definition().key.id == id,
        "C9ExactSelectorIdentity:" + id);
    check(!c8a_error([&] { select_backend_registration({native}, "llamacpp", id); }).empty(), "C9InactiveSelectorCannotFallBack:" + id);
    auto bad_seeds = seeds; bad_seeds[1].backend_selector = "cpu";
    check(!build_backend_catalog(CatalogAdmissionBuilder::test_with_seeds(binding_fixture::builtins(false, &doc, false, &owner), bad_seeds), {}).catalog,
        "C9SelectorCannotWiden:" + id);
    bad_seeds = seeds; bad_seeds.push_back(seeds[1]);
    check(!build_backend_catalog(CatalogAdmissionBuilder::test_with_seeds(binding_fixture::builtins(false, &doc, false, &owner), bad_seeds), {}).catalog,
        "C9DuplicateSelectorRejected:" + id);
    CatalogPublicationGuard publication(catalog_publication());
    publication.commit(catalog.catalog->data(), catalog.catalog);
    RuntimeConfig config(Json{{"auto_evict", false}, {"inhibit_suspend", false}, {"log_level", "error"}, {"max_loaded_models", 4}});
    auto* previous = RuntimeConfig::global(); RuntimeConfig::set_global(&config);
    {
        Router router(&config, nullptr, nullptr);
        ModelInfo model; model.recipe = "llamacpp";
        RecipeOptions options("llamacpp", Json::object()); options.set_option("llamacpp_backend", id);
        std::unique_ptr<WrappedServer> server;
        const auto creation_error = c8a_error([&] { server = RoutingHelperTestHook::create_backend(router, model, &options); });
        check(creation_error.empty() && dynamic_cast<ConfiguredServer*>(server.get()) && server->get_descriptor() == selected->descriptor && native_calls == 0,
            "C9RouterDispatchesSharedSelector:" + id);
        options.set_option("llamacpp_backend", "cpu");
        const auto error = c8a_error([&] { (void)RoutingHelperTestHook::create_backend(router, model, &options); });
        check(error == "CPU primary factory control" && native_calls == 1, "C9RouterNativeControl:" + id);
    }
    RuntimeConfig::set_global(previous);
}
