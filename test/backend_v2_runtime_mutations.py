import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

RUNNER = "src/cpp/server/backends/configured/backend_definition_runner.cpp"
SERVICES = "src/cpp/server/backends/configured/backend_runtime_services.cpp"


def prior_mutations():
    return [
        (
            "chain-order",
            RUNNER,
            "program_.hooks[a].order < program_.hooks[b].order",
            "program_.hooks[a].order > program_.hooks[b].order",
            "definition_runner",
            "RequestChainsFollowSealedOrder",
        ),
        (
            "effect",
            SERVICES,
            "if (std::find(effects_.begin(), effects_.end(), effect) == effects_.end())",
            "if (false)",
            "runtime_services",
            "ServiceCannotEscapeEffectSet",
        ),
        (
            "active-slot",
            SERVICES,
            " || frame_.active_slot != slot_",
            "",
            "runtime_services",
            "ServiceOutsideBoundSlotRejects",
        ),
        (
            "thread",
            SERVICES,
            "thread_ != std::this_thread::get_id() || ",
            "",
            "runtime_services",
            "ServiceOnForeignThreadRejects",
        ),
        (
            "executable",
            SERVICES,
            "frame_.executable != executable_ || frame_.nonce != nonce_ ||",
            "frame_.nonce != nonce_ ||",
            "runtime_services",
            "ServiceOnForeignExecutableRejects",
        ),
        (
            "owner",
            SERVICES,
            "if (owner_.owner_kind != kind || owner_.owner_id != owner || owner_.stage != stage)",
            "if (false)",
            "runtime_services",
            "WrongOwnerRejects",
        ),
        (
            "nested",
            SERVICES,
            "if (frame_.active_slot) throw",
            "if (false) throw",
            "runtime_services",
            "NestedSlotRejects",
        ),
        (
            "allowance",
            SERVICES,
            "if (allowance > remaining_)",
            "if (false)",
            "runtime_services",
            "ServiceAllowanceIsBounded",
        ),
        (
            "facts",
            SERVICES,
            "if (input.producer ? !frame_.produced.count(*input.producer) : !frame_.facts.count(input.fact))",
            "if (false)",
            "runtime_services",
            "MissingProducerIsNotLazilyInvoked",
        ),
        (
            "produced-facts",
            SERVICES,
            "input.producer ? !frame_.produced.count(*input.producer)",
            "input.producer ? false",
            "runtime_services",
            "MissingProducedFactDoesNotInvokeBackendLazily",
        ),
        (
            "cancellation",
            SERVICES,
            "    check_cancelled(cancel_);",
            "",
            "runtime_services",
            "EveryEffectChecksCancellation",
        ),
        (
            "nonce",
            SERVICES,
            "nonce(invocation_nonce())",
            "nonce(1)",
            "runtime_services",
            "InvocationNoncesAreUnique",
        ),
        (
            "resource",
            SERVICES,
            "if (std::find(supported_.begin(), supported_.end(), backend) == supported_.end() &&\n        std::find(supported_.begin(), supported_.end(), input.backend) == supported_.end())",
            "if (false)",
            "runtime_services",
            "SelectionCannotEscapeBackendScope",
        ),
        (
            "runtime-hook-id-redirect",
            RUNNER,
            "hook.slot.owner_kind == kind && hook.slot.owner_id == owner && hook.slot.stage == stage",
            "hook.slot.stage == stage && hook.slot.hook_id == program_.hooks.front().slot.hook_id",
            "definition_runner",
            "RunnerEndToEndDispatchAndFrame",
        ),
        (
            "response-cell",
            RUNNER,
            'slots(SlotOwnerKind::Operation, op.id, "response")',
            'slots(SlotOwnerKind::Operation, program_.operations.front().id, "response")',
            "definition_runner",
            "ResponseChainStaysOnSelectedCell",
        ),
        (
            "request-frame",
            RUNNER,
            'frame.data.values["request"] = request;',
            'frame.data.values["request"] = json::object();',
            "definition_runner",
            "RunnerEndToEndDispatchAndFrame",
        ),
        (
            "raw-stream",
            RUNNER,
            "if (original_body && op.identity && op.remove.empty() && op.set.empty() && chain.empty())",
            "if (false)",
            "definition_runner",
            "EmptyStreamingChainRemainsEmptyAndRaw",
        ),
        (
            "stream-chain",
            RUNNER,
            'const auto chain = slots(SlotOwnerKind::Operation, op.id, "request");',
            'const auto chain = delivery == OperationDelivery::Streaming ? std::vector<std::size_t>{} : slots(SlotOwnerKind::Operation, op.id, "request");',
            "definition_runner",
            "StreamingRequestChainUsesBoundCell",
        ),
        (
            "explicit-empty",
            RUNNER,
            'if (!normalize && existing != frame.data.options.end() && *existing == "" && frame.explicit_options.count(name)) return;',
            "",
            "runtime_services",
            "RuntimeDefaultsPreserveExplicitEmptyAndNull",
        ),
        (
            "explicit-null",
            RUNNER,
            "if (!normalize && existing != frame.data.options.end() && existing->is_null()) return;",
            'if (!normalize && existing != frame.data.options.end() && existing->is_null()) frame.data.options[name] = "";',
            "runtime_services",
            "RuntimeDefaultsPreserveExplicitEmptyAndNull",
        ),
        (
            "merge-args",
            RUNNER,
            '(item.respect_merge_args && !(normalize ? normalized_merge : frame.data.options.value("merge_args", true)))',
            "false",
            "runtime_services",
            "RuntimeDefaultsRespectMergeArgs",
        ),
        (
            "default-stage",
            RUNNER,
            "item.stage != stage || ",
            "",
            "runtime_services",
            "DefaultsApplyAtDeclaredStage",
        ),
        (
            "context-floor",
            RUNNER,
            "if (floor != launch.context_floor.end())",
            "if (false)",
            "definition_runner",
            "ContextFloorDoesNotMutateCaller",
        ),
        (
            "spec-ownership",
            RUNNER,
            'if (spec.executable_definition.get() != executable_.get() || !valid_spec_aliases(*executable_, spec) || spec.definition ||\n        spec.effective_public_recipe() != declaration()["recipe"] || spec.effective_pin_recipe() != spec.effective_public_recipe())',
            "if (false)",
            "definition_runner",
            "LeafSpecOwnershipBeforeEffects",
        ),
        (
            "adapter-kind",
            RUNNER,
            "if (!installed ||",
            "if (false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-mask",
            RUNNER,
            "!description.model_types || (description.model_types & ~k_all_operation_model_types) ||",
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-method",
            RUNNER,
            'op.method != (operation == BackendOperation::Slots ? "GET" : "POST") ||',
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-port",
            RUNNER,
            '!port || op.port != readiness["port"] ||',
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-endpoint",
            RUNNER,
            "!endpoint(op.path) ||",
            "false ||",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
        (
            "adapter-timeout",
            RUNNER,
            "op.timeout_seconds > static_cast<std::uint64_t>(std::numeric_limits<long>::max())",
            "false",
            "runtime_boundary",
            "UnsupportedAdapterHasNoEntry",
        ),
    ]


ARTIFACTS = "src/cpp/server/backends/configured/backend_artifact_services.cpp"
CAPTURE = "src/cpp/server/utils/process_manager.cpp"


def mutations():
    return prior_mutations() + [
        (
            "bounded-header-metadata",
            ARTIFACTS,
            'result.context_length = scalar(".context_length");',
            "result.context_length = 0;",
            "runtime_services",
            "BoundedHeaderPopulatesTypedMetadataAndCapabilityLabels",
        ),
        (
            "external-staging-privilege",
            RUNNER,
            "executable_->compiled().authorized().definition().key.space == DefinitionNamespace::Builtin",
            "true",
            "runtime_services",
            "ExternalLeafNeverGetsBuiltinExternalStagingAuthority",
        ),
        (
            "external-managed-staging",
            RUNNER,
            "!external_program ||\n                    ",
            "",
            "runtime_services",
            "ExternalLeafCanStageItsManagedRoot",
        ),
        (
            "install-phase-dispatch",
            RUNNER,
            "services.install_backend(spec.effective_public_recipe(), backend);",
            "(void)0;",
            "runtime_services",
            "InstallPhaseUsesLeafIdentityBeforeProgram",
        ),
        (
            "auxiliary-owner",
            RUNNER,
            'const auto owner = role == "mmproj" || role == "draft" ? "main" : role;',
            "const auto owner = role;",
            "runtime_services",
            "AuxiliaryResolutionUsesBoundArtifactOwnerAndActiveInventory",
        ),
        (
            "active-snapshot-relative-input",
            "src/cpp/server/backends/configured/typed/llamacpp_artifacts.cpp",
            "entry.active && entry.relative_path.substr(entry.relative_path.find('/', 10) + 1) == input.variant",
            "false",
            "runtime_services",
            "AuxiliaryResolutionUsesBoundArtifactOwnerAndActiveInventory",
        ),
        (
            "install-when-laziness",
            RUNNER,
            "if (expression.kind == Kind::When)",
            "if (false)",
            "runtime_services",
            "InstallFactsFollowWhenAndCoalesceBranches:rocm-stable",
        ),
        (
            "install-coalesce-laziness",
            RUNNER,
            "if (expression.kind == Kind::Coalesce && child.evaluate(frame.data, program_.bindings)) break;",
            "if (false) break;",
            "runtime_services",
            "InstallFactsFollowWhenAndCoalesceBranches:cuda",
        ),
        (
            "install-all-laziness",
            RUNNER,
            "if (expression.kind == Kind::All && !value) break;",
            "if (false) break;",
            "runtime_services",
            "InstallBooleanShortCircuitSkipsFacts:all",
        ),
        (
            "install-any-laziness",
            RUNNER,
            "if (expression.kind == Kind::Any && value) break;",
            "if (false) break;",
            "runtime_services",
            "InstallBooleanShortCircuitSkipsFacts:any",
        ),
        (
            "install-fact-caching",
            RUNNER,
            " && !frame.data.values.contains(json::json_pointer(name))",
            "",
            "runtime_services",
            "InstallFactIsReadOnceAcrossFilenameReferences",
        ),
        (
            "install-platform-backend",
            "src/cpp/server/backends/configured/configured_backend_ops.cpp",
            'return matches(record, "backend", backend) && matches(record, "os", platform.os) &&\n                matches(record, "machine_arch", platform.machine_arch) && record.contains("arch");',
            'return matches(record, "os", platform.os) &&\n                matches(record, "machine_arch", platform.machine_arch) && record.contains("arch");',
            "runtime_services",
            "InstallPlatformFiltersBackendAndOsBeforeArchitecture",
        ),
        (
            "install-platform-os",
            "src/cpp/server/backends/configured/configured_backend_ops.cpp",
            'return matches(record, "backend", backend) && matches(record, "os", platform.os) &&\n                matches(record, "machine_arch", platform.machine_arch) && record.contains("arch");',
            'return matches(record, "backend", backend) && \n                matches(record, "machine_arch", platform.machine_arch) && record.contains("arch");',
            "runtime_services",
            "InstallPlatformFiltersBackendAndOsBeforeArchitecture",
        ),
        (
            "install-platform-machine",
            "src/cpp/server/backends/configured/configured_backend_ops.cpp",
            'return matches(record, "backend", backend) && matches(record, "os", platform.os) &&\n                matches(record, "machine_arch", platform.machine_arch) && record.contains("arch");',
            'return matches(record, "backend", backend) && matches(record, "os", platform.os) &&\n                record.contains("arch");',
            "runtime_services",
            "InstallPlatformFiltersMachineBeforeArchitecture",
        ),
        (
            "install-platform-eligible",
            "src/cpp/server/backends/configured/configured_backend_ops.cpp",
            'return matches(record, "backend", backend) && matches(record, "os", platform.os) &&\n                matches(record, "machine_arch", platform.machine_arch) && record.contains("arch");',
            "return false;",
            "runtime_services",
            "InstallPlatformQueriesArchitectureForEligibleCase",
        ),
        (
            "probe-effect-before-io",
            SERVICES,
            "    guard_.check(typed::Effect::SystemProbe);",
            "",
            "runtime_services",
            "ArtifactOnlyHrxCannotObtainRuntimeOrSystemEffects",
        ),
        (
            "parent-effect-before-io",
            SERVICES,
            "    guard_.check(typed::Effect::ParentEnvironmentRead);",
            "",
            "runtime_services",
            "ArtifactOnlyHrxCannotObtainRuntimeOrSystemEffects",
        ),
        (
            "runtime-effect-before-io",
            SERVICES,
            "    guard_.check(typed::Effect::RuntimeFactRead);",
            "",
            "runtime_services",
            "ArtifactOnlyHrxCannotObtainRuntimeOrSystemEffects",
        ),
        (
            "install-effect-before-io",
            SERVICES,
            "    guard_.check(typed::Effect::InstallFactRead);",
            "",
            "runtime_services",
            "InstallAuthorityCheckedBeforeReadingFacts",
        ),
        (
            "artifact-effect-before-open",
            ARTIFACTS,
            "    check();\n    if (approved_path.empty()",
            "    if (approved_path.empty()",
            "runtime_services",
            "ArtifactAuthorityCheckedBeforeOpeningInventory",
        ),
        (
            "retained-lifetime-is-not-authority",
            SERVICES,
            "return current && current->load(std::memory_order_acquire);",
            "return bool(current);",
            "runtime_services",
            "ExpiredArtifactHandleRejects",
        ),
        (
            "install-filter-backend",
            "src/cpp/server/backends/configured/backend_definition_runner.cpp",
            "return found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();",
            'return axis != "backend" && found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();',
            "runtime_services",
            "CaseSelectorsRunBeforeFacts:backend",
        ),
        (
            "install-filter-os",
            "src/cpp/server/backends/configured/backend_definition_runner.cpp",
            "return found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();",
            'return axis != "os" && found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();',
            "runtime_services",
            "CaseSelectorsRunBeforeFacts:os",
        ),
        (
            "install-filter-machine_arch",
            "src/cpp/server/backends/configured/backend_definition_runner.cpp",
            "return found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();",
            'return axis != "machine_arch" && found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();',
            "runtime_services",
            "CaseSelectorsRunBeforeFacts:machine_arch",
        ),
        (
            "install-filter-arch",
            "src/cpp/server/backends/configured/backend_definition_runner.cpp",
            "return found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();",
            'return axis != "arch" && found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();',
            "runtime_services",
            "CaseSelectorsRunBeforeFacts:arch",
        ),
        (
            "manager-context-owner",
            "src/cpp/server/backends/configured/configured_v2_backend_ops.cpp",
            "InstallPhase::Repository, spec.executable_definition};",
            "InstallPhase::Repository, {}};",
            "runtime_services",
            "BackendManagerPhaseConsumerUsesRepositoryThenResolvedAsset",
        ),
        (
            "manager-asset-phase",
            "src/cpp/server/backends/configured/configured_v2_backend_ops.cpp",
            "context.phase = InstallPhase::Asset;",
            "context.phase = InstallPhase::Repository;",
            "runtime_services",
            "BackendManagerPhaseConsumerUsesRepositoryThenResolvedAsset",
        ),
        (
            "manager-resolved-tag",
            "src/cpp/server/backends/configured/configured_v2_backend_ops.cpp",
            "context.resolved_version = resolve_version(repository.repo);",
            "context.resolved_version = resolve_version(repository.repo); context.resolved_version = context.pinned_version;",
            "runtime_services",
            "BackendManagerPhaseConsumerUsesRepositoryThenResolvedAsset",
        ),
        (
            "leaf-pin-owner",
            "src/cpp/server/backends/configured/configured_v2_backend_ops.cpp",
            "shipped_pins.value(spec.effective_pin_recipe(), json::object())",
            'shipped_pins.value("other-recipe", json::object())',
            "runtime_services",
            "LeafPinOwnershipAndShippedDefaultPrecedence",
        ),
        (
            "artifact-nul-path",
            "src/cpp/server/backends/configured/backend_artifact_services.cpp",
            " || approved_path.find('\\0') != std::string::npos",
            "",
            "runtime_services",
            "ArtifactInputCannotHidePathAfterNul",
        ),
        (
            "artifact-entry-limit",
            "src/cpp/server/backends/configured/backend_artifact_services.cpp",
            "++visited > 2048",
            "++visited > 4096",
            "runtime_services",
            "ArtifactEntryLimitBoundsEmptyDirectoryTraversal",
        ),
        (
            "artifact-depth-limit",
            "src/cpp/server/backends/configured/backend_artifact_services.cpp",
            "iterator.depth() > 16",
            "iterator.depth() > 32",
            "runtime_services",
            "ArtifactDepthIsBounded",
        ),
        (
            "staging-architecture",
            "src/cpp/server/backends/configured/backend_runtime_services.cpp",
            " || !architectures_.count(action.architecture)",
            "",
            "runtime_services",
            "StagingRejectsArchitectureOutsideRuntimeQuery",
        ),
        (
            "foreign-artifact",
            ARTIFACTS,
            "const auto token = index(file);",
            "const auto token = [&] { try { return index(file); } catch (...) { return inventory_->entries.at(1).token; } }();",
            "runtime_services",
            "ForeignArtifactHandleRejects",
        ),
        (
            "ineligible-system-probe",
            SERVICES,
            'if (input_.backend != "system" || (linux_only && (!input_.binary_found || input_.platform != typed::Platform::Linux)))',
            "if (false)",
            "runtime_services",
            "SystemServiceCannotProbeArbitraryBackend",
        ),
        (
            "repository-asset-fact",
            SERVICES,
            "input_.phase != typed::InstallPhase::Asset &&\n        std::find(input_.repository_facts.begin(), input_.repository_facts.end(), fact) == input_.repository_facts.end()",
            "false",
            "runtime_services",
            "RepositoryServiceRejectsAssetFact",
        ),
        (
            "unrequested-install-fact",
            SERVICES,
            "std::find(input_.requested.begin(), input_.requested.end(), fact) == input_.requested.end()",
            "false",
            "runtime_services",
            "InstallServiceRejectsUnrequestedFact",
        ),
        (
            "eager-repository-expression",
            RUNNER,
            "if (context.phase == InstallPhase::Asset || full) {\n                    request",
            "if (true) {\n                    request",
            "runtime_services",
            "RepositoryDoesNotEvaluateSelectedAssetExpression",
        ),
        (
            "json-staging-path",
            RUNNER,
            "destination = services.staging_directory(plan.command.program);",
            'destination = options.value("staging_path", services.staging_directory(plan.command.program));',
            "runtime_services",
            "JsonPathCannotSupplyStagingAuthority",
        ),
        (
            "foreign-staging",
            SERVICES,
            "!owns(action.capability) || ",
            "",
            "runtime_services",
            "ForeignStagingCapabilityRejects",
        ),
        (
            "expired-staging",
            SERVICES,
            "if (!invocation_active(plan.lifetime_))",
            "if (false)",
            "runtime_services",
            "ExpiredStagingEnvelopeRejects",
        ),
        (
            "parent-enumeration",
            SERVICES,
            "if (key != typed::ParentKey::Path && key != typed::ParentKey::LlamaDevice && key != typed::ParentKey::MetalNoResidency && key != typed::ParentKey::Home && key != typed::ParentKey::LibraryPath &&\n        key != typed::ParentKey::CudaVisibleDevices && key != typed::ParentKey::PrimeRenderOffload)",
            "if (false)",
            "runtime_services",
            "EnvironmentRejectsNonEnumeratedParentKey",
        ),
        (
            "runtime-enumeration",
            SERVICES,
            "if (query != typed::RuntimeQuery::RocmArchitecture && query != typed::RuntimeQuery::TheRockDirectory &&\n        query != typed::RuntimeQuery::CudaVisibleDevices && query != typed::RuntimeQuery::HomeDirectory && query != typed::RuntimeQuery::CudaArchitecture)",
            "if (false)",
            "runtime_services",
            "EnvironmentRejectsNonEnumeratedRuntimeQuery",
        ),
        (
            "probe-read-cap",
            SERVICES,
            "sources_.version(65536, 5000,",
            "sources_.version(std::numeric_limits<std::size_t>::max(), 5000,",
            "runtime_services",
            "ProbeReadAndWaitAreBounded",
        ),
        (
            "probe-wait-cap",
            SERVICES,
            "sources_.version(65536, 5000,",
            "sources_.version(65536, std::numeric_limits<unsigned>::max(),",
            "runtime_services",
            "ProbeReadAndWaitAreBounded",
        ),
        (
            "expired-artifact",
            ARTIFACTS,
            "if (!invocation_active(lifetime_))",
            "if (false)",
            "runtime_services",
            "ExpiredArtifactHandleRejects",
        ),
        (
            "artifact-nonce",
            ARTIFACTS,
            'if (found == inventory_->entries.end()) throw std::logic_error("Foreign or expired artifact handle");',
            "if (found == inventory_->entries.end()) return 0;",
            "runtime_services",
            "ReusedServiceCannotAcceptExpiredArtifactHandle",
        ),
        (
            "artifact-prefix-limit",
            ARTIFACTS,
            "if (bytes > 262144)",
            "if (false)",
            "runtime_services",
            "ArtifactPrefixIsBounded",
        ),
        (
            "artifact-result-inventory",
            ARTIFACTS,
            'throw std::logic_error("Returned artifact path outside approved handle inventory");',
            "return path;",
            "runtime_services",
            "ReturnedPathMustBelongToInventory",
        ),
        (
            "hf-relationship",
            ARTIFACTS,
            'if (fs::is_symlink(status) && (relative.rfind("snapshots/", 0) != 0 || target.parent_path() != root / "blobs"))',
            "if (false)",
            "runtime_services",
            "ArtifactLinkRequiresApprovedSnapshotBlobRelationship",
        ),
        (
            "capture-byte-limit",
            CAPTURE,
            "result.byte_limit = bytes > max_bytes - result.output.size();",
            "result.byte_limit = false;",
            "runtime_services",
            "ByteCaptureBoundsNewlineFreeCombinedOutput",
        ),
        (
            "capture-deadline",
            CAPTURE,
            "result.timed_out = std::chrono::steady_clock::now() >= deadline;",
            "result.timed_out = false;",
            "runtime_services",
            "ByteCaptureWaitIsBounded",
        ),
        (
            "probe-arbitrary-executable",
            SERVICES,
            'const char* binary = "llama-server";',
            'const char* binary = "other-engine";',
            "runtime_services",
            "FixedProbeNeverUsesShellOrArbitraryExecutable",
        ),
        (
            "probe-shell-string",
            SERVICES,
            'utils::ProcessManager::capture_output(executable, {"--version"}, bytes, wait, cancel)',
            'utils::ProcessManager::capture_output("/bin/sh", {"-c", executable + " --version"}, bytes, wait, cancel)',
            "runtime_services",
            "FixedProbeNeverUsesShellOrArbitraryExecutable",
        ),
    ]


def run(command, cwd, log, env, timeout=120):
    with log.open("w") as output:
        try:
            return subprocess.run(
                command,
                cwd=cwd,
                env=env,
                stdout=output,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            ).returncode
        except subprocess.TimeoutExpired:
            return 124


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--unit", choices=["unit1"], required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build, scratch, out = (
        p.resolve() for p in (args.build_dir, args.scratch, args.out)
    )
    for path in (scratch, out):
        if path == Path("/tmp") or Path("/tmp") in path.parents:
            raise ValueError("Runtime evidence must use persistent project storage")
        path.mkdir(parents=True, exist_ok=True)
    temp = scratch / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    commands = json.loads((build / "compile_commands.json").read_text())
    results = []
    for mutation in mutations():
        name, relative, before, after, suite, assertion, *extra = mutation
        directory = scratch / name
        directory.mkdir(exist_ok=True)
        source = root / relative
        text = source.read_text()
        if before not in text:
            raise ValueError(f"Missing mutation anchor: {name}")
        text = text.replace(before, after)
        if extra:
            text = text.replace(extra[0], extra[1])
        copied = directory / source.name
        copied.write_text(text)
        record = {
            "rule": name,
            "assertion": assertion,
            "source_sha256": hashlib.sha256(copied.read_bytes()).hexdigest(),
        }
        candidates = [x for x in commands if x["file"] == str(source)]
        entry = next(
            (x for x in candidates if "lemonade-typed-hook-test.dir" in x["command"]),
            candidates[0],
        )
        command = shlex.split(entry["command"])
        original_object = command[command.index("-o") + 1]
        command[command.index("-o") + 1] = str(directory / "mutant.o")
        command[command.index("-c") + 1] = str(copied)
        for flag in ("-MT", "-MF"):
            if flag in command:
                index = command.index(flag)
                del command[index : index + 2]
        command = [x for x in command if x != "-MD"]
        command += ["-I", str(source.parent), "-O0"]
        record["compile"] = run(command, build, directory / "compile.log", env)
        if record["compile"] == 0:
            target = "test_backend_" + suite
            lines = subprocess.check_output(
                ["ninja", "-C", str(build), "-t", "commands", target], text=True
            ).splitlines()
            link = shlex.split(lines[-1])
            if link[:2] == [":", "&&"]:
                link = link[2:-2]
            link = [
                str(directory / "mutant.o") if x == original_object else x
                for x in link
                if not x.startswith("-Wl,--dependency-file=")
            ]
            if str(directory / "mutant.o") not in link:
                link.insert(1, str(directory / "mutant.o"))
            link[link.index("-o") + 1] = str(directory / target)
            record["link"] = run(link, build, directory / "link.log", env)
            if record["link"] == 0:
                record["run"] = run(
                    [str(directory / target)], root, directory / "run.log", env, 60
                )
                record["caught"] = (
                    record["run"] == 1
                    and "FAIL " + assertion in (directory / "run.log").read_text()
                )
        results.append(record)
        (out / "mutations.json").write_text(
            json.dumps(
                {"coverage": "4d1-runner-and-services", "mutations": results}, indent=2
            )
            + "\n"
        )
        print(
            f"{name}: {'caught' if record.get('caught') else 'NOT CAUGHT'}", flush=True
        )
    if not all(x.get("caught") for x in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
