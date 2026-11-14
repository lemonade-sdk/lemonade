#pragma once

#include "c8a_contracts.h"

void c8a_checkpoint() {
    const auto fixture = Json::parse(read_bytes("c8a.ds4.json"));
    const auto doc = fixture.at("definition");
    const auto value = c8a_candidate(doc);
    check(bool(value.compiled), "C8aCheckpointCompiles");
    if (!value.compiled) { diagnostics(compile_execution_definition(value.token).diagnostics); return; }
    const auto bound = bind(value);
    check(bool(bound.definition), "C8aCheckpointBinds"); if (!bound.definition) return;
    const auto entry = ConfiguredBackendEntry::make(bound.definition);
    DefinitionRunner runner(bound.definition);
    Services services;
    const auto recipe = doc.at("recipe").get<std::string>();
    for (const auto& [backend, policy] : entry->descriptor.containers)
        services.pins[policy.pin.recipe][policy.pin.backend] = "fixture@sha256:" + std::string(64, 'a');
    const auto scratch = fs::path(LEMON_C8A_SCRATCH) / "checkpoint";
    fs::create_directories(scratch / "directory.fake");
    const auto fallback = (scratch / "checkpoint.fake").string(); std::ofstream(fallback) << "checkpoint";
    const auto resolved = (scratch / "resolved.fake").string(); std::ofstream(resolved) << "resolved";
    ModelInfo model; model.recipe = recipe; model.model_name = "fixture"; model.type = ModelType::LLM;
    RecipeOptions options(recipe, Json::object()); options.set_option(recipe + "_backend", ""); options.set_option(recipe + "_args", ""); options.set_option("ctx_size", 8192);
    for (const auto& path : {std::string(), (scratch / "absent.fake").string()}) {
        model.resolved_paths["main"] = path; model.checkpoints["main"] = fallback;
        services.events.clear();
        const auto prepared = runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services);
        check(prepared.plan.container && prepared.plan.container->mounts.size() == 1 &&
            prepared.plan.container->mounts[0].source == fs::canonical(fallback).string() &&
            prepared.plan.container_path_rewrites.count(fallback) == 1 &&
            prepared.session->frame.values.at("artifacts").at("main") == "/mnt/models/checkpoint.fake",
            "C8aMissingResolvedUsesAbsoluteCheckpoint");
        check(services.events == std::vector<std::string>{"port", "install"}, "C8aCheckpointRetainsPreparationOrder");
    }
    model.resolved_paths["main"] = resolved; model.checkpoints["main"] = fallback;
    auto prepared = runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services);
    check(prepared.plan.container && prepared.plan.container->mounts.size() == 1 &&
        prepared.plan.container->mounts[0].source == fs::canonical(resolved).string() &&
        prepared.plan.container_path_rewrites.count(resolved) == 1 && prepared.plan.container_path_rewrites.count(fallback) == 0,
        "C8aAvailableResolvedAlwaysWins");
    for (const auto& checkpoint : {std::string(), (scratch / "absent-checkpoint.fake").string(), fs::relative(fallback).string()}) {
        for (const auto& path : {std::string(), (scratch / "absent.fake").string()}) {
            model.resolved_paths["main"] = path; model.checkpoints["main"] = checkpoint; services.events.clear();
            const auto error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
            check(error == "ds4: no local GGUF found for model 'fixture' (checkpoint: " + checkpoint + ")" && services.events.empty(), "C8aCheckpointStillFailsWithoutExistingAbsolute");
        }
        model.resolved_paths["main"] = resolved;
        check(c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); }).empty(),
            "C8aResolvedDoesNotRequireValidCheckpoint");
    }
    model.resolved_paths["main"] = ""; model.checkpoints["main"] = (scratch / "directory.fake").string(); services.events.clear();
    auto error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
    check(error == "Invalid container artifact kind" && std::find(services.events.begin(), services.events.end(), "install") == services.events.end(),
        "C8aFallbackCannotBypassArtifactInventory");
    model.resolved_paths["main"] = (scratch / "directory.fake").string(); model.checkpoints["main"] = fallback;
    error = c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); });
    check(error == "Invalid container artifact kind", "C8aExistingResolvedDirectoryDoesNotFallback");
    model.resolved_paths["main"] = ""; model.checkpoints["main"] = fallback + std::string("\0suffix", 7);
    check(!c8a_error([&] { runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); }).empty(), "C8aCheckpointInventoryRejectsNul");
    auto declaration = value.compiled->normalized_declaration(); declaration["artifacts"]["main"].erase("checkpoint_consumer");
    check(!AuthorityInspection::permits_execution(*value.token, declaration), "C8aCheckpointConsumerSealed");
    check(!c8a_candidate(doc, false).compiled, "C8aCheckpointNeedsGrant");
    for (const auto* field : {"check", "required_when", "absolute_checkpoint_fallback", "role", "strategy"}) {
        auto changed = doc;
        if (std::string(field) == "check") changed["merge"]["artifacts"]["main"][field] = "nonempty";
        else if (std::string(field) == "role") changed["merge"]["artifacts"]["main"][field] = "draft";
        else if (std::string(field) == "strategy") changed["merge"]["artifacts"]["main"][field] = "hook";
        else changed["merge"]["artifacts"]["main"][field] = false;
        check(c8a_rejected(changed), "C8aCheckpointClosedShape:" + std::string(field));
    }
    for (bool conditional : {false, true}) {
        auto changed = doc;
        if (conditional) changed["merge"]["launch"]["mounts"][0]["when"] = false;
        else changed["merge"]["launch"]["mounts"].erase(0);
        check(c8a_rejected(changed), "C8aCheckpointRequiresInventoryMount");
    }
    auto changed = doc; changed["merge"].erase("model_contract");
    check(c8a_rejected(changed), "C8aCheckpointRequiresContainerModelContract");
    changed = doc; changed["merge"]["artifacts"]["main"]["checkpoint_consumer"] = "arbitrary";
    check(c8a_rejected(changed), "C8aCheckpointClosedEnum");
    auto external = root("c8a-external-checkpoint");
    external["merge"]["artifacts"] = {{"main", {{"role", "main"}, {"strategy", "resolved_path"},
        {"absolute_checkpoint_fallback", true}, {"checkpoint_consumer", "builtin_container_absolute_v1"}, {"missing_error", "missing"}}}};
    const auto admitted = admit({external}, empty_builtins());
    check(bool(admitted.catalog), "C8aExternalCheckpointAdmittedForCompile");
    if (admitted.catalog) {
        const auto compiled = compile_execution_definition(admitted.catalog->v2_definition("c8a-external-checkpoint"));
        check(!compiled.definition && has_code(compiled.diagnostics, "unsupported_execution_feature") &&
            std::any_of(compiled.diagnostics.entries.begin(), compiled.diagnostics.entries.end(), [](const auto& item) {
                return item.pointer == "/artifacts/checkpoint_consumer";
            }), "C8aExternalCheckpointCapabilityGate");
    }
    std::ofstream(scratch / "consumer.json") << Json{{"resolved_precedence", true}, {"fallback_inventory_required", true}}.dump(2) << '\n';
}
