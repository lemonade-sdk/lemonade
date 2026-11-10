#pragma once

Fixture c8a_candidate(const Json& doc, bool granted = true) {
    const auto id = doc.at("id").get<std::string>();
    auto declaration = doc.at("merge"); declaration["recipe"] = doc.at("recipe");
    const auto semantics = container_execution_owner(declaration);
    const BuiltinContainerExecutionOwner owner{{id, 1}, semantics, v2::identity("lemonade.backend-container-owner.v1\n", semantics)};
    const auto admitted = admit({}, builtins(false, &doc, false, granted ? &owner : nullptr));
    if (!admitted.catalog) { diagnostics(admitted.diagnostics); throw std::logic_error("C8a admission failed"); }
    const auto token = admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, id, 1});
    const auto compiled = compile_execution_definition(token);
    return {admitted.catalog, token, compiled.definition};
}
template<class Call> std::string c8a_error(Call call) {
    try { call(); } catch (const std::exception& error) { return error.what(); }
    return "";
}
bool c8a_rejected(const Json& doc) {
    try { return !c8a_candidate(doc).compiled; } catch (const std::exception&) { return true; }
}
void c8a_pre_install() {
    for (const auto* name : {"halobox", "nathanw"}) {
        const auto fixture = Json::parse(read_bytes("c8a." + std::string(name) + ".json"));
        const auto doc = fixture.at("definition");
        const auto value = c8a_candidate(doc);
        check(bool(value.compiled), "C8aRefusalCompiles:" + std::string(name));
        if (!value.compiled) { return; }
        const auto bound = bind(value);
        check(bool(bound.definition), "C8aRefusalBinds:" + std::string(name));
        if (!bound.definition) { return; }
        const auto entry = ConfiguredBackendEntry::make(bound.definition);
        Services services; services.install_failure = true;
        services.pins = {{"llamacpp", {{name, "fixture@sha256:" + std::string(64, 'a')}}}};
        ModelInfo model; model.recipe = "llamacpp"; model.model_name = "fixture"; model.type = ModelType::LLM;
        model.extras["hf_load"] = true;
        RecipeOptions options("llamacpp", Json::object());
        options.set_option("llamacpp_backend", ""); options.set_option("llamacpp_args", ""); options.set_option("ctx_size", 8192);
        DefinitionRunner runner(bound.definition);
        const auto expected = "Model 'fixture' has llama-server download its own weights, which llamacpp:" + std::string(name) +
            " cannot do from its container's private network. Load it on a llamacpp backend that runs on the host.";
        auto error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
        check(error == expected && services.events.empty(), "C8aRefusalBeforeAnyInstall:" + std::string(name));
        const auto scratch = fs::path(LEMON_C8A_SCRATCH) / name; fs::create_directories(scratch);
        std::ofstream(scratch / "refusal.json") << Json{{"error", error}, {"events", services.events}}.dump(2) << '\n';
        for (bool explicit_false : {false, true}) {
            model.extras.erase("hf_load"); if (explicit_false) model.extras["hf_load"] = false;
            services.events.clear();
            error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
            check(error == "Fake install failure" && services.events == std::vector<std::string>{"install"},
                "C8aNonHfStillInstalls:" + std::string(name));
        }
        services.install_failure = false; services.events.clear();
        model.resolved_paths["main"] = (scratch / "main.fake").string();
        std::ofstream(scratch / "main.fake") << "fixture";
        error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
        check(error.empty() && std::count(services.events.begin(), services.events.end(), "install") == 1,
            "C8aNonHfInstallsExactlyOnce:" + std::string(name));
        services.install_failure = true;
        auto declaration = value.compiled->normalized_declaration(); declaration["launch"].erase("pre_install_refusal");
        check(!AuthorityInspection::permits_execution(*value.token, declaration), "C8aRefusalPolicySealed");
        declaration = value.compiled->normalized_declaration(); std::swap(declaration["launch"]["prepare_order"][0], declaration["launch"]["prepare_order"][1]);
        check(!AuthorityInspection::permits_execution(*value.token, declaration), "C8aRefusalOrderSealed");
        check(!c8a_candidate(doc, false).compiled, "C8aRefusalNeedsGrant");
        auto changed = doc; changed["merge"]["launch"]["pre_install_refusal"] = "arbitrary";
        check(!c8a_error([&] { c8a_candidate(changed); }).empty(), "C8aRefusalClosedEnum");
        changed = doc; changed["merge"]["launch"]["prepare_order"] = {"install", "backend", "hook", "artifacts", "program", "ports"};
        check(c8a_rejected(changed), "C8aRefusalRequiresBackendFirst");
        changed = doc; changed["merge"]["launch"]["prepare_order"] = {"backend", "hook", "artifacts", "program", "ports"};
        check(c8a_rejected(changed), "C8aRefusalRequiresInstallPhase");
        changed = doc; changed["merge"]["launch"].erase("diagnostics");
        const auto no_logs = c8a_candidate(changed); const auto no_logs_bound = bind(no_logs);
        const auto no_logs_entry = ConfiguredBackendEntry::make(no_logs_bound.definition);
        declaration = no_logs.compiled->normalized_declaration();
        std::swap(declaration["launch"]["prepare_order"][0], declaration["launch"]["prepare_order"][1]);
        check(!AuthorityInspection::permits_execution(*no_logs.token, declaration), "C8aRefusalOrderSealedWithoutDiagnostics");
        services.events.clear();
        error = c8a_error([&] { DefinitionRunner(no_logs_bound.definition).prepare_load({"fixture", model, options, "error"}, no_logs_entry->spec, services); });
        check(error == "Fake install failure" && services.events == std::vector<std::string>{"install"}, "C8aRefusalInstallationIndependentOfLogs");
    }
    auto external = root("c8a-external-refusal");
    external["merge"]["install"] = Json::parse(read_bytes("binding.synthetic.json")).at("merge").at("install");
    external["merge"]["launch"]["pre_install_refusal"] = "container_hf_load_v1";
    external["merge"]["launch"]["prepare_order"] = {"backend", "install", "artifacts", "program", "ports"};
    const auto admitted = admit({external}, empty_builtins());
    if (!admitted.catalog) diagnostics(admitted.diagnostics);
    check(admitted.catalog && !compile_execution_definition(admitted.catalog->v2_definition("c8a-external-refusal")).definition,
        "C8aExternalRefusalRejected");
}
