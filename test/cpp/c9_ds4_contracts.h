#pragma once

void c9_ds4_contracts(const std::string& id, const Json& doc, const Fixture& fixture,
    const std::shared_ptr<const ExecutableDefinition>& bound, Services& services, ModelInfo model, RecipeOptions options) {
    const auto c8a = Json::parse(read_bytes("c8a.ds4.json"));
    check(doc.at("merge").at("artifacts").at("main") == c8a.at("definition").at("merge").at("artifacts").at("main"),
        "C9ExactC8aCheckpointConsumer:" + id);
    const auto entry = ConfiguredBackendEntry::make(bound);
    DefinitionRunner runner(bound);
    model.min_resident_gb = 16;
    services.free_memory = 0; services.install_failure = true; services.events.clear();
    const auto prepare = [&] { return runner.prepare_load({"fixture", model, options, "error"}, entry->spec, services); };
    auto error = c8a_error(prepare);
    check(error == "Fake install failure" && services.events == std::vector<std::string>{"install"}, "C9Ds4InstallBeforeMemory:" + id);
    const auto install_error = error;
    const auto install_events = services.events;
    services.install_failure = false; services.events.clear();
    error = c8a_error(prepare);
    check(error == "ds4 needs 53.0 GiB of free GPU memory to start, and 0.0 GiB is free" &&
        services.events == std::vector<std::string>{"install", "port", "memory"}, "C9Ds4PortBeforeMemory:" + id);
    const auto memory_error = error;
    const auto memory_events = services.events;
    auto no_logs = doc; no_logs["merge"]["launch"].erase("diagnostics");
    const auto no_logs_fixture = c8a_candidate(no_logs); const auto no_logs_bound = bind(no_logs_fixture);
    const auto no_logs_entry = ConfiguredBackendEntry::make(no_logs_bound.definition);
    services.install_failure = true; services.events.clear();
    error = c8a_error([&] { DefinitionRunner(no_logs_bound.definition).prepare_load({"fixture", model, options, "error"}, no_logs_entry->spec, services); });
    check(error == "Fake install failure" && services.events == std::vector<std::string>{"install"}, "C9Ds4InstallIndependentOfDiagnostics:" + id);
    services.install_failure = false;
    services.free_memory = 120;
    const auto scratch = fs::path(LEMON_C9_SCRATCH) / id;
    fs::create_directories(scratch / "directory.fake");
    std::ofstream(scratch / "memory-ordering.json") << Json{{"install_failure", {{"error", install_error}, {"events", install_events}}},
        {"memory_failure", {{"error", memory_error}, {"events", memory_events}}}}.dump(2) << '\n';
    const auto resolved = (scratch / "resolved.fake").string(); std::ofstream(resolved) << "fake";
    const auto fallback = (scratch / "checkpoint.fake").string(); std::ofstream(fallback) << "fake";
    for (const auto& path : {std::string(), (scratch / "absent.fake").string()}) {
        model.resolved_paths["main"] = path; model.checkpoints["main"] = fallback;
        const auto prepared = prepare();
        check(prepared.plan.container && prepared.plan.container->mounts.size() == 1 &&
            prepared.plan.container->mounts[0].source == fs::canonical(fallback).string(), "C9MissingResolvedUsesAbsoluteCheckpoint:" + id);
    }
    model.resolved_paths["main"] = resolved; model.checkpoints["main"] = fallback;
    const auto selected = prepare();
    check(selected.plan.container && selected.plan.container->mounts.size() == 1 &&
        selected.plan.container->mounts[0].source == fs::canonical(resolved).string() &&
        selected.plan.container_path_rewrites.count(fallback) == 0, "C9AvailableResolvedAlwaysWins:" + id);
    for (const auto& checkpoint : {std::string(), (scratch / "absent-checkpoint.fake").string(), fs::relative(fallback).string()}) {
        for (const auto& path : {std::string(), (scratch / "absent.fake").string()}) {
            model.resolved_paths["main"] = path; model.checkpoints["main"] = checkpoint; services.events.clear();
            error = c8a_error(prepare);
            check(error == "ds4: no local GGUF found for model 'fixture' (checkpoint: " + checkpoint + ")" && services.events.empty(),
                "C9CheckpointStillFailsWithoutExistingAbsolute:" + id);
        }
    }
    model.resolved_paths["main"] = ""; model.checkpoints["main"] = (scratch / "directory.fake").string();
    error = c8a_error(prepare);
    check(error == "Invalid container artifact kind", "C9FallbackCannotBypassArtifactInventory:" + id);
    model.resolved_paths["main"] = (scratch / "directory.fake").string(); model.checkpoints["main"] = fallback;
    error = c8a_error(prepare);
    check(error == "Invalid container artifact kind", "C9ExistingResolvedDirectoryDoesNotFallback:" + id);
    model.resolved_paths["main"] = ""; model.checkpoints["main"] = fallback + std::string("\0suffix", 7);
    check(!c8a_error(prepare).empty(), "C9CheckpointRejectsNul:" + id);
    for (const auto* role : {"main", "draft"}) {
        fs::create_directories(scratch / role); const auto file = scratch / role / "checkpoint.fake";
        std::ofstream(file) << "fake"; model.resolved_paths[role] = file.string();
    }
    services.pool = 128;
    auto prepared = prepare();
    std::ofstream(scratch / "resident-plan.json") << Json{{"argv", prepared.plan.command.args}, {"env", prepared.plan.command.env}, {"mounts", prepared.plan.container->mounts.size()}}.dump(2);
    const std::vector<std::string> resident{"-m", "/mnt/models/checkpoint.fake", "--host", "0.0.0.0", "--port", std::to_string(services.port),
        "-c", "8192", "--prefill-chunk", "2048", "--dspark", "--mtp-model", "/mnt/models/2/checkpoint.fake", "--fixture", "7"};
    check(prepared.plan.command.args == resident && prepared.plan.container && prepared.plan.container->mounts.size() == 2 &&
        prepared.plan.command.env == std::vector<std::pair<std::string, std::string>>{{"HIP_VISIBLE_DEVICES", "0"}}, "C9ResidentDraftAndNumberedMounts:" + id);
    services.pool = 62;
    prepared = prepare();
    std::ofstream(scratch / "streaming-plan.json") << Json{{"argv", prepared.plan.command.args}, {"env", prepared.plan.command.env}, {"mounts", prepared.plan.container->mounts.size()}}.dump(2);
    const std::vector<std::string> streaming{"-m", "/mnt/models/checkpoint.fake", "--host", "0.0.0.0", "--port", std::to_string(services.port),
        "-c", "8192", "--prefill-chunk", "2048", "--ssd-streaming", "--ssd-streaming-cache-experts", "24GB", "--fixture", "7"};
    check(prepared.plan.command.args == streaming && prepared.plan.container && prepared.plan.container->mounts.size() == 1 &&
        prepared.plan.command.env == std::vector<std::pair<std::string, std::string>>{{"DS4_ROCM_STREAM_MODEL_CACHE_GB", "12"}, {"DS4_ROCM_STREAM_FREE_RESERVE_GB", "37"}, {"HIP_VISIBLE_DEVICES", "0"}},
        "C9StreamingArgvEnvironmentAndNoDraft:" + id);
    model.min_resident_gb = 8;
    check(runner.auto_context_kv_budget_gib(model, 50, services) == std::optional<double>(5), "C9PrelaunchAutomaticContextBudget:" + id);
    for (const auto* level : {"error", "info", "debug", "trace"}) {
        prepared = runner.prepare_load({"fixture", model, options, level}, entry->spec, services);
        check(prepared.plan.output.inherit == (std::string(level) == "info" || std::string(level) == "debug") &&
            prepared.plan.output.filter_health_logs && prepared.plan.command.ready_endpoint == "/v1/models" && prepared.plan.readiness_timeout_seconds == 2,
            "C9Ds4SourceOutputAndReadiness:" + id + ":" + level);
    }
    for (const auto& flag : doc.at("merge").at("launch").at("custom_args").at("reserved")) {
        auto invalid = options; invalid.set_option("ds4_args", flag.get<std::string>() + "=forbidden");
        error = c8a_error([&] { runner.prepare_load({"fixture", model, invalid, "error"}, entry->spec, services); });
        check(error.rfind("Invalid custom ds4-server arguments:\n", 0) == 0, "C9Ds4ReservedArgument:" + flag.get<std::string>());
    }
    auto changed = fixture.compiled->normalized_declaration(); changed["artifacts"]["main"].erase("checkpoint_consumer");
    check(!AuthorityInspection::permits_execution(*fixture.token, changed), "C9CheckpointConsumerOwnerSealed:" + id);
    changed = fixture.compiled->normalized_declaration(); changed["launch"]["execution"]["cases"][0].erase("memory_phase");
    check(!AuthorityInspection::permits_execution(*fixture.token, changed), "C9MemoryPhaseOwnerSealed:" + id);
    changed = doc; changed["merge"]["launch"]["execution"]["cases"][0]["memory_phase"] = "arbitrary";
    check(c8a_rejected(changed), "C9MemoryPhaseClosedEnum:" + id);
    changed = doc; changed["merge"]["launch"]["prepare_order"] = {"backend", "install", "artifacts", "program", "ports"};
    check(c8a_rejected(changed), "C9MemoryPhaseFixedOrder:" + id);
    changed = doc; changed["merge"].erase("model_contract");
    changed["merge"]["artifacts"]["main"].erase("checkpoint_consumer");
    changed["merge"]["artifacts"]["main"].erase("absolute_checkpoint_fallback");
    check(c8a_rejected(changed), "C9MemoryPhaseNeedsDs4Contract:" + id);
}
