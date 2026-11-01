#pragma once

#include "c7a_load_log.h"

namespace {
struct BudgetServices final : LogServices {
    IntegratedMemoryPool pool{true, 8.0, 62.0};
    std::optional<ArtifactByteFact> main{ArtifactByteFact{true, 64ULL * 1073741824ULL}};
    std::optional<ArtifactByteFact> draft{ArtifactByteFact{true, 0}};
    mutable unsigned pools = 0, sizes = 0;
    explicit BudgetServices(std::vector<std::string>& events) : LogServices(events) {}
    IntegratedMemoryPool integrated_memory_pool() const override { ++pools; return pool; }
    std::optional<ArtifactByteFact> artifact_bytes(const std::string& path) const override { ++sizes; return path == "main.fake" ? main : draft; }
    FreeMemoryFact free_gpu_memory() const override { throw std::logic_error("Prelaunch context must use the caller's available memory"); }
};
void context_budget_contract() {
    const auto fixture = Json::parse(read_bytes("c7a.budget.json"));
    const auto doc = fixture.at("definition");
    const auto value = candidate(doc);
    const auto bound = executable(value);
    auto runner = std::make_shared<const DefinitionRunner>(bound);
    std::vector<std::string> events;
    auto services = std::make_shared<BudgetServices>(events);
    ConfiguredV2BackendOps ops(runner, services);
    ModelInfo model; model.recipe = "gufo"; model.type = ModelType::LLM; model.device = DEVICE_GPU;
    model.resolved_paths["main"] = "main.fake"; model.min_resident_gb = 8; model.size = 64;
    model.max_context_window = 131072; model.gguf.block_count = 32; model.gguf.head_count_kv = 256; model.gguf.key_length = 128;
    check(ops.auto_context_kv_budget_gib(model, 50) == std::optional<double>(5), "C7aSourceBudgetIsFiveGiB");
    check(compute_auto_context_size(model, 50, false, true, &ops) == 40960, "C7aEstimatorUsesEarlierBudgetInsteadOfZero");
    check(compute_auto_context_size(model, 50, false, true, default_backend_ops()) == 0, "C7aGenericCounterexampleStillZero");
    for (const auto& row : fixture.at("budget_cases")) {
        const auto pool = row.at("pool").get<double>();
        services->pool = {pool > 0, 0.0, pool};
        services->main = ArtifactByteFact{true, row.at("main_gib").get<std::uint64_t>() * 1073741824ULL};
        services->draft = ArtifactByteFact{true, row.at("draft_gib").get<std::uint64_t>() * 1073741824ULL};
        model.resolved_paths["draft"] = services->draft->bytes ? "draft.fake" : "";
        model.min_resident_gb = row.at("working_gib"); services->pools = services->sizes = 0;
        const auto actual = ops.auto_context_kv_budget_gib(model, row.at("available_gib"));
        check(row.at("budget_gib").is_null() ? !actual : actual == std::optional<double>(row.at("budget_gib").get<double>()), "C7aSourceContextBudget:" + row.at("name").get<std::string>());
        check(services->pools == 1 && services->sizes == (model.resolved_path("draft").empty() ? 1u : 2u) && events.empty(), "C7aBudgetBeforeLaunchEffects:" + row.at("name").get<std::string>());
    }
    services->pool = {true, 8.0, 62.0}; services->main = ArtifactByteFact{true, 60ULL * 1073741824ULL}; services->draft = ArtifactByteFact{true, 4ULL * 1073741824ULL};
    model.resolved_paths["draft"] = "draft.fake"; model.min_resident_gb = 8;
    check(compute_auto_context_size(model, 50, false, true, &ops) == 32768, "C7aDraftKvStillChargedOnce");
    model.labels = {"mtp"};
    check(compute_auto_context_size(model, 50, false, true, &ops) == 30624, "C7aMtpAndDraftKvAccountingPreserved");
    model.labels.clear(); model.resolved_paths["draft"] = "";
    services->main = ArtifactByteFact{false, 0};
    check(ops.auto_context_kv_budget_gib(model, 50) == std::optional<double>(5), "C7aObservedStatFailureRetainsSourceWorkingSet");
    for (const auto* missing : {"available", "vram", "virtual", "main", "draft"}) {
        services->pool = {true, 8.0, 62.0}; services->main = ArtifactByteFact{true, 60ULL * 1073741824ULL}; services->draft = ArtifactByteFact{true, 4ULL * 1073741824ULL};
        model.resolved_paths["draft"] = "draft.fake";
        const std::string name = missing;
        if (name == "available") services->pool.available.reset();
        if (name == "vram") services->pool.vram_gib.reset();
        if (name == "virtual") services->pool.virtual_gib.reset();
        if (name == "main") services->main.reset();
        if (name == "draft") services->draft.reset();
        check(refuses([&] { ops.auto_context_kv_budget_gib(model, 50); }), "C7aMissingBudgetFactFailsClosed:" + name);
    }
    services->pool = {true, 8.0, 62.0}; services->main = ArtifactByteFact{true, 64ULL * 1073741824ULL}; model.resolved_paths["draft"] = "";
    for (const auto available : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1e20})
        check(refuses([&] { ops.auto_context_kv_budget_gib(model, available); }), "C7aAvailableBudgetInputBounded");
    services->pool.virtual_gib = std::numeric_limits<double>::infinity();
    check(refuses([&] { ops.auto_context_kv_budget_gib(model, 50); }), "C7aPoolBudgetInputBounded");
    for (const auto budget : {51.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        check(estimate_auto_context_budget(50, 64, 131072, true, false, true, budget).max_tokens == 0, "C7aSuppliedBudgetBoundedByAvailable");
    check(estimate_auto_context_budget(50, 64, 131072, true, false, true, -3.0).max_tokens == 0, "C7aNegativeBudgetCannotCreateCapacity");
    auto metadata_model = model; metadata_model.labels = {"reasoning", "mtp"};
    typed::ArtifactMetadata metadata{typed::ArtifactHeader{"ds4", 131072, {}}, {"embedding", "vision"}};
    runner->apply_artifact_metadata(metadata_model, metadata);
    check(metadata_model.labels == std::vector<std::string>{"reasoning", "mtp"} && metadata_model.gguf.architecture == "ds4" && metadata_model.max_context_window == 131072, "C7aDs4MetadataPreservesRegisteredLabels");
    auto unselected = doc; unselected["merge"].erase("model_contract");
    const auto generic_runner = std::make_shared<const DefinitionRunner>(executable(candidate(unselected)));
    services->pools = services->sizes = 0;
    ConfiguredV2BackendOps generic(generic_runner, services);
    check(!generic.auto_context_kv_budget_gib(model, 50) && services->pools == 0 && services->sizes == 0, "C7aUnselectedBudgetHasNoEffects");
    generic_runner->apply_artifact_metadata(metadata_model, metadata);
    check(metadata_model.labels == metadata.labels, "C7aOtherMetadataBehaviorUnchanged");
    check(!candidate(doc, false).compiled, "C7aUngrantedBudgetRejected");
    auto declaration = value.compiled->normalized_declaration(); declaration.erase("model_contract");
    check(!AuthorityInspection::permits_execution(*value.token, declaration), "C7aBudgetSelectionSealed");
    auto external = doc; external["id"] = external["recipe"] = "c7a-budget-external";
    const auto rejected = admit({external}, empty_builtins());
    check(!rejected.catalog && has_code(rejected.diagnostics, "container_execution_unavailable"), "C7aExternalBudgetRejected");
    auto changed = doc; changed["merge"]["launch"]["backend"]["fixed"] = "vulkan";
    check(!candidate(changed).compiled, "C7aBudgetLimitedToFixedRocm");
    changed = doc; changed["merge"]["launch"]["execution"]["cases"][0].erase("memory_plan");
    check(!candidate(changed).compiled, "C7aBudgetRequiresSameLaunchMemoryPlan");
}
}
