"""Run the CPU-only final gates after acquiring the shared test-suite queue."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
SCRATCH = Path("/tmp/lemonade-unit4c-remainder")
BASE = "1ea315ae32a21d79c0e551a957a19f68023a1709"
commands = [
    (
        "cpp-ci",
        [
            "ctest",
            "--test-dir",
            "build",
            "-L",
            "^cpp-ci$",
            "--output-on-failure",
            "--timeout",
            "180",
        ],
    )
]
for suite in [
    "builtin_authorization",
    "owner_graph",
    "exports",
    "authorization",
    "v2_contract",
    "resolver",
    "resolver_bounds",
    "resolver_diagnostics",
    "descriptor",
    "v1_compat",
    "execution",
    "execution_bounds",
    "execution_boundary",
]:
    commands.append((suite, ["build/test_backend_definition_" + suite]))
commands.append(("abi", ["build/test_backend_hook_abi"]))
for suite in ["admission", "invalidation", "admission_bounds"]:
    commands.append((suite, ["build/test_backend_catalog_" + suite]))
for name, script, arguments in [
    ("catalog-http", "backend_catalog_http.py", ["build/lemond"]),
    ("frozen-cli", "backend_schema_cli.py", ["build/lemonade"]),
    ("cli-repl", "cli_repl_options.py", ["build/lemond", "build/lemonade"]),
    ("negotiation", "backend_catalog_negotiation.py", ["build/lemond"]),
    (
        "snapshot-after",
        "backend_v1_snapshot.py",
        [
            "capture",
            "--lemond",
            "build/lemond",
            "--cli",
            "build/lemonade",
            "--out",
            str(OUT / "after"),
        ],
    ),
    (
        "snapshot-compare",
        "backend_v1_snapshot.py",
        [
            "compare",
            "--lemond",
            "build/lemond",
            "--cli",
            "build/lemonade",
            "--baseline",
            str(OUT / "before"),
        ],
    ),
    (
        "v1-projections",
        "backend_v1_phase3_compare.py",
        [
            "--baseline",
            str(OUT / "before/snapshot.json"),
            "--actual",
            str(OUT / "after/snapshot.json"),
            "--out",
            str(OUT / "comparison"),
        ],
    ),
    (
        "data-parity",
        "backend_definition_data_parity.py",
        [
            "--build-dir",
            "build",
            "--baseline-commit",
            BASE,
            "--views",
            "native",
            "windows",
            "macos",
            "--out",
            str(SCRATCH / "data-parity"),
            "--mode",
            "run",
        ],
    ),
    (
        "compiler-purity",
        "backend_execution_purity.py",
        [
            "--nm",
            "/usr/bin/nm",
            "--library",
            "build/liblemonade-backend-definition-execution.a",
        ],
    ),
    (
        "loader-symbols",
        "backend_definition_builtin_audit.py",
        ["symbols", "--build", "build", "--out", str(SCRATCH / "symbols")],
    ),
    (
        "builtin-mutations",
        "backend_definition_builtin_audit.py",
        [
            "mutations",
            "--build",
            "build",
            "--out",
            str(SCRATCH / "builtin-mutations"),
            "--run-only",
        ],
    ),
    (
        "additional-mutations",
        "backend_definition_builtin_audit.py",
        [
            "mutations",
            "--build",
            "build",
            "--out",
            str(SCRATCH / "additional-mutations"),
            "--run-only",
        ],
    ),
    (
        "owner-mutations",
        "backend_definition_owner_audit.py",
        [
            "mutations",
            "--build",
            "build",
            "--out",
            str(SCRATCH / "owner-mutations"),
            "--run-only",
        ],
    ),
]:
    commands.append((name, [sys.executable, "test/" + script, *arguments]))
for script in ["backend-catalog", "recipe-form", "schema-recipe-options"]:
    commands.append(
        ("renderer-" + script, ["npm", "--prefix", "src/app", "run", "test:" + script])
    )
commands.extend(
    [
        (
            "generator",
            [
                sys.executable,
                "docs/tools/gen_backend_boilerplate.py",
                "--lemond",
                "build/lemond",
                "--check",
            ],
        ),
        (
            "whole-snapshot",
            [
                "cmp",
                str(OUT / "before/snapshot.json"),
                str(OUT / "after/snapshot.json"),
            ],
        ),
        (
            "oracle-equality",
            [
                sys.executable,
                "test/llamacpp_oracle_audit.py",
                "equality",
                "--build",
                "build",
                "--out",
                str(SCRATCH / "oracle-equality"),
            ],
        ),
        (
            "oracle-mutations",
            [
                sys.executable,
                "test/llamacpp_oracle_audit.py",
                "mutations",
                "--build",
                "build",
                "--out",
                str(SCRATCH / "oracle-mutations"),
            ],
        ),
        ("smoke", [sys.executable, str(OUT / "smoke.py")]),
        (
            "protected-source",
            [
                "git",
                "diff",
                "--exit-code",
                BASE,
                "--",
                "schemas/backend-definition-v1.schema.json",
                "src/cpp/resources/backends",
                "src/cpp/resources/defaults.json",
                "src/cpp/server/backends/configured/backend_definition_store.cpp",
                "src/cpp/server/backends/configured/configured_backend_ops.cpp",
                "src/cpp/server/backends/configured/configured_backend_entry.cpp",
                "cmake/BackendDescriptorFromDefinition.cmake",
                "src/cpp/server/backends/llamacpp/llamacpp_server.cpp",
                "src/cpp/server/backends/llamacpp/llamacpp_request.cpp",
                "test/fixtures/llamacpp-oracle",
                "test/fixtures/backend-v2/export.synthetic.json",
                "test/cpp/test_backend_definition_authorization.cpp",
            ],
        ),
        ("diff-check", ["git", "diff", "--check"]),
    ]
)
results = []
for name, command in commands:
    start = time.monotonic()
    process = subprocess.run(
        command, cwd=ROOT, capture_output=True, text=True, timeout=1200
    )
    log = process.stdout + process.stderr
    (OUT / (name + ".log")).write_text(log)
    results.append(
        dict(
            name=name,
            command=command,
            returncode=process.returncode,
            seconds=round(time.monotonic() - start, 2),
            tail=log.splitlines()[-3:],
        )
    )
    (OUT / "gates.json").write_text(json.dumps(results, indent=2) + "\n")
    print(name, process.returncode, results[-1]["seconds"], flush=True)
    if name == "cpp-ci":
        shutil.copyfile(
            ROOT / "build/Testing/Temporary/LastTest.log", OUT / "cpp-ci-output.log"
        )
    if process.returncode:
        print(log[-6000:], flush=True)
        sys.exit(1)
