#include "backend_runtime_fixture.h"

namespace lemon::backends::runtime {
struct TestAccess {
    static std::unique_ptr<InvocationGuard> guard(Frame& frame, const ExecutableDefinition& executable,
        std::size_t index, const execution::SymbolicHook& hook, SlotOwnerKind kind, const std::string& owner,
        const std::string& stage, utils::RequestCancelToken cancel = {}) {
        return std::unique_ptr<InvocationGuard>(new InvocationGuard(frame, executable, index, hook, kind, owner, stage, std::move(cancel)));
    }
};
}

using namespace runtime_fixture;

int main() {
    try {
    const auto executable = runtime_fixture::executable();
    const auto& program = ex::Inspection::program(executable->compiled());
    const auto at = std::find_if(program.hooks.begin(), program.hooks.end(), [](const auto& hook) { return hook.contract == typed::Contract::Backend; });
    const auto index = static_cast<std::size_t>(at - program.hooks.begin());
    rt::Frame frame(*executable);
    frame.facts.insert(typed::Fact::BackendOptions);
    auto guard = rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend");
    rt::SelectionServices service(*guard, {"cpu"});
    check(service.supports("cpu") && !service.supports("cuda"), "SelectionUsesCapturedLeafSupport");
    check(throws([&] { service.validate_device({"cuda", "CUDA0"}); }, "scope"), "SelectionCannotEscapeBackendScope");
    check(throws([&] { guard->check(typed::Effect::ArtifactRead); }, "Effect outside"), "ServiceCannotEscapeEffectSet");
    frame.active_slot.reset();
    check(throws([&] { service.supports("cpu"); }, "Inactive"), "ServiceOutsideBoundSlotRejects");
    frame.active_slot = index;
    const auto identity = frame.executable;
    frame.executable = nullptr;
    check(throws([&] { service.supports("cpu"); }, "Inactive"), "ServiceOnForeignExecutableRejects");
    frame.executable = identity;
    check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend"); }, "Nested"), "NestedSlotRejects");
    auto other_thread = std::async(std::launch::async, [&] { return throws([&] { service.supports("cpu"); }, "Inactive"); });
    check(other_thread.get(), "ServiceOnForeignThreadRejects");
    check(throws([&] { guard->check(typed::Effect::SupportQuery, 1024 * 1024 + 1); }, "allowance"), "ServiceAllowanceIsBounded");
    guard.reset();
    check(!frame.active_slot, "GuardExpiresOnReturn");
    rt::Frame other(*executable);
    check(other.nonce != frame.nonce, "InvocationNoncesAreUnique");
    std::atomic<bool> cancelled{false};
    guard = rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend", {&cancelled, {}});
    rt::SelectionServices cancelled_service(*guard, {"cpu"});
    cancelled = true;
    check(throws([&] { cancelled_service.supports("cpu"); }, "cancelled"), "EveryEffectChecksCancellation");
    guard.reset();
    for (const auto& item : {std::pair<SlotOwnerKind, std::string>{SlotOwnerKind::Operation, "backend"}, {SlotOwnerKind::Root, "system"}})
        check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, item.first, item.second, "backend"); }, "owner/stage"), "WrongOwnerRejects");
    check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "request"); }, "owner/stage"), "WrongStageRejects");
    frame.facts.clear();
    check(throws([&] { rt::TestAccess::guard(frame, *executable, index, *at, SlotOwnerKind::Root, "backend", "backend"); }, "Missing"), "MissingProducerIsNotLazilyInvoked");
    const auto prepare = std::find_if(program.hooks.begin(), program.hooks.end(), [](const auto& hook) { return hook.contract == typed::Contract::Prepare; });
    for (const auto& input : prepare->inputs) if (!input.producer) frame.facts.insert(input.fact);
    check(throws([&] { rt::TestAccess::guard(frame, *executable, static_cast<std::size_t>(prepare - program.hooks.begin()), *prepare,
        SlotOwnerKind::Root, "launch", "launch"); }, "Missing"), "MissingProducedFactDoesNotInvokeBackendLazily");
    const auto entry = ConfiguredBackendEntry::make(executable);
    auto info = model();
    RecipeOptions launch_only;
    entry->backend_ops()->resolve_runtime_options(info, launch_only);
    check(launch_only.get_option("fork_args") == "", "DefaultsApplyAtDeclaredStage");
    for (const auto& value : {Json(""), Json(nullptr), Json("--user 2")}) {
        RecipeOptions options; options.set_option("fork_args", value); options.set_option("merge_args", false);
        entry->backend_ops()->resolve_runtime_options(info, options);
        check(options.get_explicit_option("fork_args") == value, "RuntimeOptionsPreserveExplicitEmptyNullAndMergeArgs");
    }
    auto document = runtime_document();
    document["merge"]["launch"]["custom_args"]["defaults"][0]["stage"] = "runtime_options";
    const auto defaults = ConfiguredBackendEntry::make(runtime_fixture::executable(document));
    RecipeOptions omitted;
    defaults->backend_ops()->resolve_runtime_options(info, omitted);
    check(omitted.get_option("fork_args") == "--parallel 1", "DefaultsApplyAtDeclaredStage");
    for (const auto& value : {Json(""), Json(nullptr)}) {
        RecipeOptions explicit_value; explicit_value.set_option("fork_args", value);
        defaults->backend_ops()->resolve_runtime_options(info, explicit_value);
        check(explicit_value.get_explicit_option("fork_args") == value, "RuntimeDefaultsPreserveExplicitEmptyAndNull");
    }
    RecipeOptions no_merge; no_merge.set_option("merge_args", false);
    defaults->backend_ops()->resolve_runtime_options(info, no_merge);
    check(no_merge.get_option("fork_args") == "", "RuntimeDefaultsRespectMergeArgs");
    for (const auto& argument : {"--parallel 4", "-np 4"}) {
        auto aliases = document;
        aliases["merge"]["launch"]["custom_args"]["defaults"][0]["aliases"] = {"-np"};
        const auto alias_entry = ConfiguredBackendEntry::make(runtime_fixture::executable(aliases));
        RecipeOptions selected; selected.set_option("fork_args", argument);
        alias_entry->backend_ops()->resolve_runtime_options(info, selected);
        check(selected.get_option("fork_args") == argument, "RuntimeDefaultAliasesPreserveOverride");
    }
    for (const auto& name : {"install_context", "environment", "system"}) {
        auto unsupported = runtime_document();
        unsupported["merge"]["hooks"][name] = binding_fixture::document()["merge"]["hooks"][name];
        const auto bound = runtime_fixture::executable(unsupported);
        check(throws([&] { ConfiguredBackendEntry::make(bound); }, "consumer is unavailable"), std::string("UnimplementedConsumerFailsClosed:") + name);
    }
    } catch (const std::exception& error) {
        std::printf("Runtime rejection: %s\n", error.what());
        check(false, "RuntimeServicesReachNamedBoundaries");
    }
    return finish();
}
