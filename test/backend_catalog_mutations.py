import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

LEDGER = "src/cpp/server/backends/configured/backend_catalog_ledger.cpp"
EXECUTION = "src/cpp/server/backends/configured/backend_catalog_execution.cpp"
COMPILER = "src/cpp/server/backends/configured/backend_definition_execution.cpp"
BINDER = "src/cpp/server/backends/configured/backend_definition_binding.cpp"
PUBLICATION = "src/cpp/server/backends/backend_catalog_publication_internal.h"


def mutations():
    return [
        (
            "catalog-projection-constructs-server",
            "src/cpp/server/backends/configured/backend_catalog.cpp",
            "        const auto* entry = registration.configured_entry;",
            '        const auto* entry = registration.configured_entry;\n        if (entry) registration.create(BackendContext{"error"});',
            "execution",
            "CatalogValidationConstructsNoRuntimeServers",
        ),
        (
            "catalog-address-identity",
            "src/cpp/server/backends/configured/backend_catalog.cpp",
            'schema_["catalog_id"] = schema_digest(identity.dump());',
            'schema_["catalog_id"] = schema_digest(identity.dump() + std::to_string(reinterpret_cast<std::size_t>(this)));',
            "execution",
            "ExecutionCatalogIdentityIsDeterministic",
        ),
        (
            "compiler-receipt-failure",
            COMPILER,
            "            result.definition.reset();",
            "",
            "accounting",
            "CompilerReceiptFailureCannotReturnCompletedDefinition",
        ),
        (
            "binder-receipt-failure",
            BINDER,
            "            result.definition.reset();",
            "",
            "accounting",
            "BinderReceiptFailureCannotReturnCompletedDefinition",
        ),
        (
            "compilation-inventory-temporary",
            EXECUTION,
            "                accounting::Temporary inventory(v2::checked_sum(builtins->source_bytes_,\n                    v2::checked_product(builtins->definitions_, sizeof(DefinitionKey) + 128)));",
            "",
            "accounting",
            "CompilationReservesInventoryAlongsideProgramTemporaryStorage",
        ),
        (
            "registry-consumes-before-fallback",
            PUBLICATION,
            "        data();\n        return server();",
            "        return server();",
            "publication",
            "RegistryReadConsumesBeforeSelectingBuiltinFallback",
        ),
        (
            "duplicate-ownership-receipt",
            LEDGER,
            "if (!object || receipts_.count(object)) throw",
            "if (!object) throw",
            "accounting",
            "OwnershipReceiptCannotBeMintedTwice",
        ),
        (
            "nested-stage-restoration",
            LEDGER,
            "active = previous_; stage = previous_stage_;",
            "active = previous_;",
            "accounting",
            "StageWorkAccumulatesAndNestedScopesRestoreStage",
        ),
        (
            "finalization-temporary-charge",
            EXECUTION,
            "accounting::Temporary temporary(v2::checked_sum(v2::checked_product(ledger->metrics().stages[3].owned_bytes, 2), 8192));",
            "",
            "accounting",
            "FinalSchemaAndIdentityTemporaryReservationIsCharged",
        ),
        (
            "entry-owned-charge",
            EXECUTION,
            "accounting::owned(v2::checked_sum(v2::checked_product(bytes, 4), sizeof(ConfiguredBackendEntry) + 4096));",
            "",
            "accounting",
            "EveryEntryAndProjectionReservesOwnedStorage",
        ),
        (
            "cumulative-work-reset",
            LEDGER,
            "    metrics_.work_bytes += bytes;",
            "    metrics_.work_bytes = bytes;",
            "accounting",
            "StageWorkAccumulatesAndNestedScopesRestoreStage",
        ),
        (
            "aggregate-diagnostics",
            EXECUTION,
            "admission_limits.diagnostics = std::min(admission_limits.diagnostics, ledger->limits().diagnostics);",
            "admission_limits.diagnostics = 100;",
            "accounting",
            "DiagnosticBoundAppliesAcrossAdmissionAndCompilation",
        ),
        (
            "shared-payload-double-charge",
            LEDGER,
            "    work(0, 1);\n    ++metrics_.dependency_references;",
            "    owned(4096);\n    work(0, 1);\n    ++metrics_.dependency_references;",
            "accounting",
            "SharedDependencyPayloadIsChargedOnce",
        ),
        (
            "owned-copy-charge",
            LEDGER,
            "    metrics_.retained_bytes += bytes;",
            "    metrics_.retained_bytes += 0;",
            "accounting",
            "EqualOwnedCopiesAreChargedSeparately",
        ),
        (
            "temporary-charge",
            LEDGER,
            "if (ledger_) ledger_->temporary(bytes);",
            "if (ledger_) bytes_ = 0;",
            "accounting",
            "TemporaryPeakIncludesRetainedOwnership",
        ),
        (
            "aggregate-storage-bound",
            LEDGER,
            "    bound(metrics_.retained_bytes, bytes, limits_.retained_bytes);\n    bound(metrics_.retained_bytes + bytes, temporary_, limits_.retained_bytes);",
            "",
            "accounting",
            "OwnedStorageLimitRejectsBeforeCounterGrowth",
        ),
        (
            "aggregate-peak-bound",
            LEDGER,
            "    bound(metrics_.retained_bytes, temporary_ + bytes, limits_.retained_bytes);",
            "",
            "accounting",
            "AggregateOneUnderRejects:0",
        ),
        (
            "aggregate-work-bytes",
            LEDGER,
            "    bound(metrics_.work_bytes, bytes, limits_.work_bytes);",
            "",
            "accounting",
            "AggregateOneUnderRejects:2",
        ),
        (
            "aggregate-work-visits",
            LEDGER,
            "    bound(metrics_.work_visits, visits, limits_.work_visits);",
            "",
            "accounting",
            "AggregateOneUnderRejects:3",
        ),
        (
            "aggregate-temporary-bound",
            LEDGER,
            "    bound(temporary_, bytes, limits_.temporary_bytes);",
            "",
            "accounting",
            "AggregateOneUnderRejects:1",
        ),
        (
            "stage-storage-bound",
            LEDGER,
            "    bound(current.owned_bytes, bytes, limits_.stages[index()].owned_bytes);",
            "",
            "accounting",
            "IndependentStageBound:0:0",
        ),
        (
            "stage-work-bytes",
            LEDGER,
            "    bound(current.work_bytes, bytes, limit.work_bytes);",
            "",
            "accounting",
            "IndependentStageBound:0:1",
        ),
        (
            "stage-work-visits",
            LEDGER,
            "    bound(current.work_visits, visits, limit.work_visits);",
            "",
            "accounting",
            "IndependentStageBound:0:2",
        ),
        (
            "stage-temporary-bound",
            LEDGER,
            "    bound(temporary_, bytes, limits_.stages[index()].peak_temporary_bytes);",
            "",
            "accounting",
            "IndependentStageBound:0:3",
        ),
        (
            "foreign-receipt",
            LEDGER,
            '    if (!object || !receipts_.count(object)) throw std::logic_error("Foreign catalog dependency");',
            "",
            "accounting",
            "ReceiptsRequireExactTransactionObject",
        ),
        (
            "sealed-ledger",
            LEDGER,
            '    if (sealed_) throw std::logic_error("Catalog ledger is sealed");',
            "",
            "accounting",
            "SealedLedgerCannotChange",
        ),
        (
            "incomplete-bucket",
            LEDGER,
            "sealed_ || temporary_ || expected != finalized",
            "sealed_ || temporary_",
            "accounting",
            "BucketCannotSealWithoutEveryFinalizedEntry",
        ),
        (
            "hard-limits",
            LEDGER,
            "    limits_.retained_bytes = std::min(limits_.retained_bytes, hard.retained_bytes);",
            "",
            "accounting",
            "PublicBoundsOnlyReduceHardLimits",
        ),
        (
            "normalization-charge-order",
            COMPILER,
            "#ifdef LEMONADE_BINDING_TEST\n            ++execution::counters.declaration_copies;\n#endif\n            execution::Program staging;\n            staging.normalized = source.declaration;",
            "",
            "accounting",
            "NormalizationCopyIsChargedBeforeAllocation",
        ),
        (
            "compiler-shared-dependency",
            COMPILER,
            "v2::charge(budget.metrics.retained_bytes, source.metrics.retained_logical_bytes, budget.limits.retained_bytes);",
            "budget.keep(source.metrics.retained_logical_bytes);",
            "accounting",
            "CompilerDoesNotRebillResolvedDependencies",
        ),
        (
            "binder-shared-dependencies",
            BINDER,
            "v2::charge(budget.metrics.retained_bytes, v2::checked_sum(admitted->metrics().retained_logical_bytes, compiled->metrics().retained_bytes), budget.limits.retained_bytes);",
            "budget.keep(v2::checked_sum(admitted->metrics().retained_logical_bytes, compiled->metrics().retained_bytes));",
            "accounting",
            "BinderDoesNotRebillAdmittedAndCompiledDependencies",
        ),
        (
            "partial-candidate-on-failure",
            EXECUTION,
            "candidate = std::shared_ptr<BackendCatalog>(new BackendCatalog(BackendCatalog::Unpublished{}, nullptr));",
            "candidate = std::shared_ptr<BackendCatalog>(new BackendCatalog(BackendCatalog::Unpublished{}, nullptr)); result.catalog = candidate;",
            "publication",
            "FailingDescendantCannotPublishFinalizedPrefix",
        ),
        (
            "early-publication-visibility",
            PUBLICATION,
            "state_.store(State::Building, std::memory_order_release);",
            "state_.store(State::Published, std::memory_order_release);",
            "publication",
            "ReadersCannotObservePartiallyBuiltCatalogOrBuiltinFallback",
        ),
        (
            "skip-descendant-compilation",
            EXECUTION,
            "                for (const auto& token : tokens) {",
            "                if (tokens.size() > 1) tokens.resize(1);\n                for (const auto& token : tokens) {",
            "execution",
            "WholeMixedGraphFinalizes",
        ),
        (
            "skip-descendant-binding",
            EXECUTION,
            "                for (const auto& program : compiled) {",
            "                for (const auto& program : compiled) { if (!executables.empty()) continue;",
            "execution",
            "WholeMixedGraphFinalizes",
        ),
        (
            "skip-descendant-finalization",
            EXECUTION,
            "                for (const auto& executable : executables) finalize(executable);",
            "                for (const auto& executable : executables) { if (candidate->entries_.empty()) finalize(executable); }",
            "execution",
            "WholeMixedGraphFinalizes",
        ),
        (
            "descendant-adapter-bypass",
            "src/cpp/server/backends/configured/backend_definition_runner.cpp",
            'if (!endpoint(readiness.at("path")) || (readiness["timeout"] != "global" &&',
            'if (false || (readiness["timeout"] != "global" &&',
            "execution",
            "FailingDescendantFinalizationCannotEscapeOrPublishPrefix",
        ),
        (
            "managed-version-coverage",
            "src/cpp/server/backends/configured/backend_catalog.cpp",
            "for (const auto& backend : normalized) if (!shipped.contains(backend)",
            "for (const auto& backend : normalized) if (false && !shipped.contains(backend)",
            "execution",
            "V2ManagedVersionsCheckedBeforePublication",
        ),
        (
            "candidate-skips-graph-validation",
            "src/cpp/server/backends/configured/backend_catalog.cpp",
            '{"valid", false}',
            '{"valid", true}',
            "execution",
            "CandidateRebuildsDependencyGraphInsteadOfReusingDescendants",
        ),
        (
            "partial-publication-data-read",
            PUBLICATION,
            '        if (observed == State::Building) throw std::logic_error("Backend catalog publication is in progress");',
            "",
            "publication",
            "ReadersCannotObservePartiallyBuiltCatalogOrBuiltinFallback",
        ),
        (
            "partial-publication-server-read",
            PUBLICATION,
            '        if (observed == State::Building) throw std::logic_error("Backend catalog publication is in progress");',
            "",
            "publication",
            "ReadersCannotObservePartiallyBuiltCatalogOrBuiltinFallback",
        ),
        (
            "published-replacement",
            PUBLICATION,
            "if (state() != State::Empty) throw",
            "if (state() != State::Empty && state() != State::Published) throw",
            "publication",
            "PublishedGenerationCannotBeReplaced",
        ),
        (
            "consumed-replacement",
            PUBLICATION,
            "if (state() != State::Empty) throw",
            "if (state() != State::Empty && state() != State::BuiltinsConsumed) throw",
            "publication",
            "ReplacementAfterConsumptionRejects",
        ),
        (
            "missing-rollback",
            PUBLICATION,
            "if (!committed_) publication_.rollback();",
            "",
            "publication",
            "FailedBuildRestoresEmptyWithoutEitherRoot",
        ),
        (
            "mismatched-data-root",
            PUBLICATION,
            "        data_ = std::move(data);",
            "        data_ = std::make_shared<const BackendCatalogData>();",
            "publication",
            "OneVisibilityEventPublishesMatchingBorrowedAddresses",
        ),
    ]


def run(command, cwd, log, env):
    with log.open("w") as output:
        return subprocess.run(
            ["timeout", "180s", *command],
            cwd=cwd,
            env=env,
            stdout=output,
            stderr=subprocess.STDOUT,
            check=False,
        ).returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build, scratch, out = (
        x.resolve() for x in (args.build_dir, args.scratch, args.out)
    )
    for path in (scratch, out):
        if path == Path("/tmp") or Path("/tmp") in path.parents:
            raise ValueError("Use persistent project storage")
        path.mkdir(parents=True, exist_ok=True)
    temp = scratch / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    commands = json.loads((build / "compile_commands.json").read_text())
    results = []
    for name, relative, before, after, suite, assertion in mutations():
        if args.only and name not in args.only:
            continue
        directory = scratch / name
        directory.mkdir(exist_ok=True)
        resources = directory / "resources"
        if not resources.exists():
            resources.symlink_to(build / "resources", target_is_directory=True)
        source = root / relative
        text = source.read_text()
        if before not in text:
            raise ValueError(f"Missing mutation anchor: {name}")
        if name == "partial-publication-data-read":
            text = text.replace(before, after, 1)
        elif name == "partial-publication-server-read":
            location = text.rindex(before)
            text = text[:location] + text[location:].replace(before, after, 1)
        else:
            text = text.replace(before, after)
        if name == "normalization-charge-order":
            text = text.replace(
                "            budget.copy(source.declaration);",
                before + "\n            budget.copy(source.declaration);",
                1,
            )
        if name == "partial-candidate-on-failure":
            text = text.replace("            result.catalog.reset();", "")
        copied = directory / source.name
        copied.write_text(text)
        target = "test_backend_catalog_" + suite
        compile_source = source
        if source.suffix == ".h":
            compile_source = root / f"test/cpp/{target}.cpp"
            copied_source = directory / compile_source.name
            copied_source.write_text(compile_source.read_text())
        else:
            copied_source = copied
        entries = [x for x in commands if x["file"] == str(compile_source)]
        if relative in (COMPILER, BINDER):
            entry = next(
                x for x in entries if "lemonade-binding-test.dir" in x["command"]
            )
        else:
            entry = entries[0]
        command = shlex.split(entry["command"])
        original_object = command[command.index("-o") + 1]
        mutant_object = str(directory / "mutant.o")
        command[command.index("-o") + 1] = mutant_object
        command[command.index("-c") + 1] = str(copied_source)
        for flag in ("-MT", "-MF"):
            if flag in command:
                index = command.index(flag)
                del command[index : index + 2]
        command = [x for x in command if x != "-MD"]
        command += ["-I", str(source.parent), "-I", str(root / "test/cpp"), "-O1"]
        record = dict(
            rule=name,
            assertion=assertion,
            base_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            test_source_sha256=hashlib.sha256(
                (root / f"test/cpp/{target}.cpp").read_bytes()
            ).hexdigest(),
            source_sha256=hashlib.sha256(copied.read_bytes()).hexdigest(),
        )
        record["compile"] = run(command, build, directory / "compile.log", env)
        if record["compile"] == 0:
            lines = subprocess.check_output(
                ["ninja", "-C", str(build), "-t", "commands", target], text=True
            ).splitlines()
            link = shlex.split(lines[-1])
            if link[:2] == [":", "&&"]:
                link = link[2:-2]
            matched = original_object in link
            link = [
                mutant_object if x == original_object else x
                for x in link
                if not x.startswith("-Wl,--dependency-file=")
            ]
            if not matched:
                link.insert(1, mutant_object)
            link[link.index("-o") + 1] = str(directory / target)
            record["link"] = run(link, build, directory / "link.log", env)
            if record["link"] == 0:
                record["run"] = run(
                    [str(directory / target)], root, directory / "run.log", env
                )
                record["caught"] = (
                    record["run"] == 1
                    and "FAIL " + assertion in (directory / "run.log").read_text()
                )
        results.append(record)
        (out / "mutations.json").write_text(json.dumps(results, indent=2) + "\n")
        print(name, "caught" if record.get("caught") else "NOT CAUGHT", flush=True)
    raise SystemExit(not all(x.get("caught") for x in results))


if __name__ == "__main__":
    main()
