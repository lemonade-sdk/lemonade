#pragma once

namespace {
void device_validation_contract(const fs::path& scratch) {
    const auto parity = Json::parse(read_bytes("c6.device-validation.json"));
    const auto doc = parity.at("definition");
    auto declaration = doc.at("merge"); declaration["recipe"] = doc.at("recipe");
    const BuiltinContainerExecutionOwner owner{{"gufo", 1}, container_execution_owner(declaration), parity.at("owner_digest")};
    const auto catalog = admit({}, builtins(false, &doc, false, &owner));
    check(catalog.catalog != nullptr, "DeviceValidationFixtureAdmitted");
    if (!catalog.catalog) { diagnostics(catalog.diagnostics); return; }
    const auto token = catalog.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1});
    const auto compiled = compile_execution_definition(token);
    check(compiled.definition != nullptr, "DeviceValidationFixtureCompiled");
    if (!compiled.definition) { diagnostics(compiled.diagnostics); return; }
    const auto bound = bind_execution_definition(catalog.catalog, compiled.definition);
    check(bound.definition != nullptr, "DeviceValidationFixtureBound");
    if (!bound.definition) { diagnostics(bound.diagnostics); return; }
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    DefinitionRunner runner(bound.definition);
    allow_entries = true;
    FakeHost host;
    BackendManager manager(std::make_shared<utils::ContainerManager>(host.host()));
    BackendManagerTestAccess::pins(manager, {{"gufo", {{"halobox", "fixture@sha256:" + std::string(64, 'a')},
        {"nathanw", "fixture@sha256:" + std::string(64, 'b')}}}});
    ModelInfo info; info.recipe = "gufo"; info.resolved_paths["main"] = (scratch / "device-model.fake").string();
    std::ofstream(info.resolved_path()) << "fixture";
    for (const auto& row : parity.at("cases")) {
        const auto selector = row.at("selector").get<std::string>();
        Services services(manager); services.install = false; services.allowed_backend = selector;
        RecipeOptions options; options.set_option("fixture_engine", selector); options.set_option("fixture_device", row.at("device"));
        bool accepted = false;
        try {
            const auto prepared = runner.prepare_load({"fixture", info, options, "error", {}, {}}, entry->spec, services);
            accepted = true;
            check(prepared.session->frame.values.at("launch").at("backend") == selector && prepared.plan.container && prepared.plan.container->backend == selector &&
                prepared.plan.command.program == row.at("program") && prepared.plan.container->image == row.at("image"), "DeviceMappingPreservesSelectionAndPin:" + selector);
            check(prepared.session->frame.values.at("launch").at("backend") == selector, "DeviceMappingPreservesExpressionSelector:" + selector);
        } catch (const std::exception&) {}
        check(accepted, "MappedDeviceAccepted:" + selector);
        options.set_option("fixture_device", row.at("wrong"));
        bool wrong_rejected = false;
        try { runner.prepare_load({"fixture", info, options, "error", {}, {}}, entry->spec, services); }
        catch (const std::invalid_argument& error) { wrong_rejected = std::string(error.what()).find("contradicts backend choice") != std::string::npos; }
        check(wrong_rejected, "MappedWrongDeviceRejected:" + selector);
        options.set_option("fixture_device", "");
        bool empty_accepted = false;
        try { runner.prepare_load({"fixture", info, options, "error", {}, {}}, entry->spec, services); empty_accepted = true; }
        catch (const std::exception&) {}
        check(empty_accepted, "MappedEmptyDevicePreservesDefault:" + selector);
    }
    auto changed = token->definition().declaration;
    changed["launch"]["execution"]["cases"][0]["device_validation"]["backend"] = "vulkan";
    check(!AuthorityInspection::permits_execution(*token, changed), "DeviceValidationMappingIsSealed");
    changed = doc;
    changed["merge"].erase("hooks");
    const auto no_hook = admit({}, builtins(false, &changed, false, &owner));
    if (no_hook.catalog) {
        const auto result = compile_execution_definition(no_hook.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1}));
        check(!result.definition && has_code(result.diagnostics, "missing_device_validation_hook"), "DeviceMappingRequiresValidationConsumer");
    } else check(false, "DeviceMappingRequiresValidationConsumer");
    auto unmapped = doc;
    for (auto& record : unmapped["merge"]["launch"]["execution"]["cases"]) record.erase("device_validation");
    declaration = unmapped.at("merge"); declaration["recipe"] = unmapped.at("recipe");
    const auto unmapped_semantics = container_execution_owner(declaration);
    const BuiltinContainerExecutionOwner unmapped_owner{{"gufo", 1}, unmapped_semantics,
        v2::identity("lemonade.backend-container-owner.v1\n", unmapped_semantics)};
    const auto unmapped_catalog = admit({}, builtins(false, &unmapped, false, &unmapped_owner));
    const auto unmapped_compiled = compile_execution_definition(unmapped_catalog.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "gufo", 1}));
    const auto unmapped_bound = bind_execution_definition(unmapped_catalog.catalog, unmapped_compiled.definition);
    const auto unmapped_entry = ConfiguredBackendEntry::make(unmapped_bound.definition);
    for (const auto& row : parity.at("cases")) {
        Services services(manager); services.install = false; services.allowed_backend = row.at("selector");
        RecipeOptions options; options.set_option("fixture_engine", row.at("selector")); options.set_option("fixture_device", row.at("device"));
        bool rejected = false;
        try { DefinitionRunner(unmapped_bound.definition).prepare_load({"fixture", info, options, "error", {}, {}}, unmapped_entry->spec, services); }
        catch (const std::invalid_argument& error) { rejected = std::string(error.what()).find("contradicts backend choice") != std::string::npos; }
        check(rejected, "AbsentDeviceMappingPreservesSelectedBackend:" + row.at("selector").get<std::string>());
    }
    allow_entries = false; calls.clear();
}
}
