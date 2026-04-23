"""End-to-end tests for the *_bin keyword-config feature on Linux.

Covers config-set validation, hot-swap (with/without loaded model), the
update_available / update_required state machine in /api/v1/system-info,
GitHub-resolution caching, and offline / no_fetch_executables interactions.

Run with `uv run python test/bin_config_test.py`. Each scenario manages its own
lemond lifecycle so state is reproducible. Lemond logs land in
/tmp/lemond_bin_config.log for post-mortem inspection.
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

import requests

REPO = Path("/home/jfowers/lsdk/lemonade")
LEMOND = REPO / "build" / "lemond"
LEMONADE_HOME = Path(os.path.expanduser("~/.cache/lemonade"))
CONFIG_FILE = LEMONADE_HOME / "config.json"
BIN_DIR = LEMONADE_HOME / "bin"
LOG_FILE = Path("/tmp/lemond_bin_config.log")
PORT = 13305
BASE = f"http://127.0.0.1:{PORT}"

# Tiny model (gemma-3-270m, ~150MB) — fast to pull and load.
TEST_MODEL = "Tiny-Test-Model-GGUF"

# A llama.cpp tag whose ubuntu-vulkan-x64 asset is downloadable. b8500 is
# verified present upstream and is below the current lemonade baseline (b8766),
# so it doubles as the "below baseline" tag for the F2 force-update_required test.
EXPLICIT_TAG_OLD = "b8500"
EXPLICIT_TAG_BELOW_BASELINE = "b8500"


# --- lemond lifecycle ---------------------------------------------------------


class Lemond:
    """Context manager that boots a fresh lemond and tears it down on exit."""

    def __init__(self, env_extra: Optional[dict] = None):
        self.env_extra = env_extra or {}
        self.proc: Optional[subprocess.Popen] = None

    def __enter__(self) -> "Lemond":
        env = os.environ.copy()
        env.update(self.env_extra)
        LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        # Truncate so each scenario inspects only its own log lines.
        self.log = LOG_FILE.open("w")
        self.proc = subprocess.Popen(
            [str(LEMOND), "--port", str(PORT)],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=env,
        )
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                r = requests.get(f"{BASE}/api/v1/health", timeout=1)
                if r.status_code == 200:
                    return self
            except requests.RequestException:
                pass
            if self.proc.poll() is not None:
                self.log.flush()
                raise RuntimeError(
                    f"lemond exited early (code {self.proc.returncode}). "
                    f"Log: {LOG_FILE}"
                )
            time.sleep(0.2)
        raise TimeoutError("lemond did not become healthy within 30s")

    def __exit__(self, *_):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()

    def log_text(self) -> str:
        return LOG_FILE.read_text(errors="replace")


# --- API helpers --------------------------------------------------------------


def set_config(changes: dict, expect_status: int = 200) -> requests.Response:
    r = requests.post(f"{BASE}/internal/set", json=changes, timeout=600)
    assert (
        r.status_code == expect_status
    ), f"POST /internal/set {changes} -> {r.status_code} {r.text}"
    return r


def system_info() -> dict:
    r = requests.get(f"{BASE}/api/v1/system-info", timeout=30)
    r.raise_for_status()
    return r.json()


def backend_state(recipe: str, backend: str) -> dict:
    return (
        system_info()
        .get("recipes", {})
        .get(recipe, {})
        .get("backends", {})
        .get(backend, {})
    )


def install_backend(recipe: str, backend: str) -> requests.Response:
    """Triggers an install (and busts the system-info cache as a side effect)."""
    r = requests.post(
        f"{BASE}/api/v1/install",
        json={"recipe": recipe, "backend": backend, "stream": False},
        timeout=600,
    )
    r.raise_for_status()
    return r


def loaded_models() -> list:
    # Use /api/v1/health rather than /health — /health may return empty body
    # immediately after a load operation completes.
    r = requests.get(f"{BASE}/api/v1/health", timeout=5)
    r.raise_for_status()
    return r.json().get("all_models_loaded", [])


# --- Filesystem helpers -------------------------------------------------------


def vulkan_version_txt() -> Path:
    return BIN_DIR / "llamacpp" / "vulkan" / "version.txt"


def cpu_version_txt() -> Path:
    return BIN_DIR / "llamacpp" / "cpu" / "version.txt"


def write_version_txt(path: Path, value: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)


def reset_vulkan_install():
    """Delete the vulkan install dir so the next set_config triggers a fresh install."""
    install_dir = BIN_DIR / "llamacpp" / "vulkan"
    if install_dir.exists():
        shutil.rmtree(install_dir)


def reset_config_to_defaults():
    """Wipe vulkan_bin / cpu_bin overrides so each scenario starts clean."""
    if CONFIG_FILE.exists():
        cfg = json.loads(CONFIG_FILE.read_text())
        cfg.setdefault("llamacpp", {})
        for k in ("vulkan_bin", "cpu_bin", "rocm_bin"):
            cfg["llamacpp"][k] = "builtin"
        for top in ("offline", "no_fetch_executables"):
            cfg[top] = False
        CONFIG_FILE.write_text(json.dumps(cfg, indent=2))


# --- Pretty assertions --------------------------------------------------------


class Result:
    def __init__(self):
        self.passes: list[str] = []
        self.fails: list[tuple[str, str]] = []

    def ok(self, name: str):
        print(f"  [PASS] {name}")
        self.passes.append(name)

    def fail(self, name: str, why: str):
        print(f"  [FAIL] {name}: {why}")
        self.fails.append((name, why))

    def summary(self):
        total = len(self.passes) + len(self.fails)
        print(f"\n{len(self.passes)}/{total} passed")
        if self.fails:
            print("\nFailures:")
            for n, w in self.fails:
                print(f"  - {n}: {w}")
        return len(self.fails) == 0


# --- Scenario groups ----------------------------------------------------------


def scenario_b1_b2_b3(r: Result):
    """B1: validation accepts; B2: rejects bad path; B3: no-op on unchanged value."""
    print("\n=== B: config-set validation ===")
    reset_config_to_defaults()
    with Lemond() as lemond:
        # B1 — accepts every documented form
        for name, value in [
            ("builtin", "builtin"),
            ("empty", ""),
            ("latest", "latest"),
            ("b-tag", "b9999"),
            ("v-tag", "v1.8.2"),
            ("master-tag", "master-569-ab6afe8"),
            ("absolute-path", "/tmp"),
        ]:
            try:
                set_config({"llamacpp": {"vulkan_bin": value}})
                # Read back via /internal/config (server-side snapshot)
                got = requests.get(f"{BASE}/internal/config").json()
                actual = got.get("llamacpp", {}).get("vulkan_bin")
                if actual == value:
                    r.ok(f"B1 accepts vulkan_bin={value!r}")
                else:
                    r.fail(
                        f"B1 vulkan_bin={value!r}",
                        f"persisted {actual!r}, expected {value!r}",
                    )
            except Exception as e:
                r.fail(f"B1 vulkan_bin={value!r}", str(e))

        # B2 — rejects nonexistent path
        try:
            resp = requests.post(
                f"{BASE}/internal/set",
                json={"llamacpp": {"vulkan_bin": "/does/not/exist"}},
                timeout=10,
            )
            if resp.status_code == 400 and "path does not exist" in resp.text:
                r.ok("B2 nonexistent path rejected at config-set with new error")
            else:
                r.fail(
                    "B2 nonexistent path rejected",
                    f"status={resp.status_code} body={resp.text[:200]}",
                )
        except Exception as e:
            r.fail("B2 nonexistent path rejected", str(e))

        # B3 — same value twice doesn't fire hot-swap
        # Reset to a known state first
        set_config({"llamacpp": {"vulkan_bin": "builtin"}})
        # Wait briefly so any first-call hot-swap log lands before measurement
        time.sleep(0.5)
        log_before = lemond.log_text()
        n_swaps_before = log_before.count("*_bin config changed: llamacpp.vulkan_bin")
        # Set the SAME value again
        set_config({"llamacpp": {"vulkan_bin": "builtin"}})
        time.sleep(0.5)
        log_after = lemond.log_text()
        n_swaps_after = log_after.count("*_bin config changed: llamacpp.vulkan_bin")
        if n_swaps_after == n_swaps_before:
            r.ok("B3 no-op set does not trigger hot-swap")
        else:
            r.fail(
                "B3 no-op set",
                f"hot-swap log line count went {n_swaps_before} -> {n_swaps_after}",
            )


def scenario_c(r: Result):
    """C: hot-swap fires for explicit tag, latest, and back to builtin (no model)."""
    print("\n=== C: hot-swap (no model loaded) ===")
    reset_config_to_defaults()
    with Lemond() as lemond:
        # builtin -> explicit tag (must download a real upstream binary)
        set_config({"llamacpp": {"vulkan_bin": EXPLICIT_TAG_OLD}})
        log = lemond.log_text()
        if (
            f"hot-swapping llamacpp:vulkan" in log
            and EXPLICIT_TAG_OLD in vulkan_version_txt().read_text(errors="replace")
        ):
            r.ok(f"C1 builtin -> {EXPLICIT_TAG_OLD} installed")
        else:
            r.fail(
                f"C1 builtin -> {EXPLICIT_TAG_OLD}",
                f"version.txt={vulkan_version_txt().read_text(errors='replace') if vulkan_version_txt().exists() else 'MISSING'}",
            )

        # explicit -> latest (resolves via GitHub, downloads)
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        log = lemond.log_text()
        installed = (
            vulkan_version_txt().read_text(errors="replace").strip()
            if vulkan_version_txt().exists()
            else ""
        )
        if (
            "Resolved 'latest' for ggml-org/llama.cpp ->" in log
            and installed
            and installed != EXPLICIT_TAG_OLD
        ):
            r.ok(f"C2 explicit -> latest resolved to {installed}")
        else:
            r.fail(
                "C2 explicit -> latest",
                f"installed={installed!r} resolve in log: {'Resolved' in log}",
            )

        # latest -> builtin (downgrades back to baseline)
        set_config({"llamacpp": {"vulkan_bin": "builtin"}})
        time.sleep(0.5)
        installed = (
            vulkan_version_txt().read_text(errors="replace").strip()
            if vulkan_version_txt().exists()
            else ""
        )
        # Baseline from backend_versions.json is b8766; accept any non-latest tag
        # that matches the pinned baseline.
        baseline = json.loads(
            (REPO / "src/cpp/resources/backend_versions.json").read_text()
        )["llamacpp"]["vulkan"]
        if installed == baseline:
            r.ok(f"C3 latest -> builtin reverted to baseline {baseline}")
        else:
            r.fail(
                "C3 latest -> builtin",
                f"version.txt={installed!r} (expected {baseline})",
            )


def scenario_d(r: Result):
    """D: hot-swap with a model loaded — unload + install + reload + chat works."""
    print("\n=== D: hot-swap with model loaded ===")
    reset_config_to_defaults()
    with Lemond() as lemond:
        # Pull the tiny test model if not already present
        try:
            requests.post(
                f"{BASE}/api/v1/pull",
                json={"model_name": TEST_MODEL},
                timeout=600,
            ).raise_for_status()
        except Exception as e:
            r.fail("D pull model", str(e))
            return

        # Detect which backend the system auto-selects so the test runs on the
        # actually-supported variant (cpu on most CI / dev boxes).
        si = system_info()
        lc = si.get("recipes", {}).get("llamacpp", {}).get("backends", {})
        backend_choice = None
        for b in ("vulkan", "cpu"):
            if lc.get(b, {}).get("state") == "installed":
                backend_choice = b
                break
        if backend_choice is None:
            r.fail(
                "D pick backend",
                f"no installed llamacpp backend found: {list(lc.keys())}",
            )
            return
        bin_key = f"{backend_choice}_bin"

        # Load the model
        try:
            requests.post(
                f"{BASE}/api/v1/load",
                json={"model_name": TEST_MODEL, "llamacpp_backend": backend_choice},
                timeout=120,
            ).raise_for_status()
        except Exception as e:
            r.fail("D load model", str(e))
            return

        # Brief settle before health probe — backend subprocess sometimes
        # takes a beat to publish its status after /load returns success.
        time.sleep(1.0)
        loaded_before = [m.get("model_name") for m in loaded_models()]
        if TEST_MODEL not in loaded_before:
            r.fail("D load model", f"loaded models = {loaded_before}")
            return
        r.ok(f"D pre-swap: model loaded on {backend_choice}")

        # Hot-swap to a different version
        version_path = BIN_DIR / "llamacpp" / backend_choice / "version.txt"
        version_before = (
            version_path.read_text(errors="replace").strip()
            if version_path.exists()
            else ""
        )
        set_config({"llamacpp": {bin_key: EXPLICIT_TAG_OLD}})
        time.sleep(1.0)  # let async restore catch up
        version_after = (
            version_path.read_text(errors="replace").strip()
            if version_path.exists()
            else ""
        )

        log = lemond.log_text()
        unloaded = "Unloading " + TEST_MODEL in log
        reloaded = ("Reloaded " + TEST_MODEL in log) or (
            TEST_MODEL in [m.get("model_name") for m in loaded_models()]
        )

        if version_after == EXPLICIT_TAG_OLD and unloaded:
            r.ok(f"D unload + install: {version_before} -> {version_after}")
        else:
            r.fail(
                "D unload + install",
                f"version {version_before}->{version_after}, unload-log={unloaded}",
            )

        if reloaded:
            r.ok("D model reloaded after install")
        else:
            r.fail(
                "D reload",
                f"model not in loaded list: {[m.get('model_name') for m in loaded_models()]}",
            )

        # Chat completion to confirm model is functional on the new binary
        try:
            cc = requests.post(
                f"{BASE}/api/v1/chat/completions",
                json={
                    "model": TEST_MODEL,
                    "messages": [{"role": "user", "content": "Say hi."}],
                    "max_tokens": 8,
                    "stream": False,
                },
                timeout=120,
            )
            cc.raise_for_status()
            text = cc.json()["choices"][0]["message"]["content"]
            if isinstance(text, str) and text:
                r.ok(f"D chat completion works on new binary (got {len(text)} chars)")
            else:
                r.fail("D chat completion", f"empty content: {cc.text[:200]}")
        except Exception as e:
            r.fail("D chat completion", str(e))


def scenario_e1(r: Result):
    """E1: latest cache hit — second resolution doesn't re-query GitHub."""
    print("\n=== E1: latest GitHub cache ===")
    reset_config_to_defaults()
    reset_vulkan_install()
    with Lemond() as lemond:
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        # Set to builtin then back to latest; the second 'latest' should hit cache
        set_config({"llamacpp": {"vulkan_bin": "builtin"}})
        time.sleep(0.5)
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        log = lemond.log_text()
        # Count actual GitHub queries via the INFO-level "Resolved" line
        # (the "Resolving" debug line is suppressed at default log level).
        n_resolves = log.count("Resolved 'latest' for ggml-org/llama.cpp ->")
        if n_resolves == 1:
            r.ok("E1 cache: only 1 GitHub resolve across 2 latest pins")
        else:
            r.fail("E1 cache", f"{n_resolves} GitHub resolves observed (expected 1)")


def scenario_e2(r: Result):
    """E2: status path's null-safe BackendManager::global() guard.

    Run /api/v1/system-info before any backend manager interaction; the guard
    must let the request succeed even if the cache is empty.
    """
    print("\n=== E2: status path null-safe ===")
    reset_config_to_defaults()
    with Lemond() as lemond:
        try:
            si = system_info()
            assert "recipes" in si
            r.ok("E2 system-info responds with no latest pins set")
        except Exception as e:
            r.fail("E2 system-info", str(e))


def scenario_f1_f2_f3(r: Result):
    """F1/F2: update_available vs forced update_required for latest pins.
    F3: regression — builtin pin still emits update_required on mismatch."""
    print("\n=== F: update_available / update_required ===")
    reset_config_to_defaults()

    # F1 — set latest, force version.txt to a tag known to be older than
    # GitHub-latest but >= baseline. We use the lemonade-shipped baseline tag
    # directly; GitHub-latest will be newer.
    baseline = json.loads(
        (REPO / "src/cpp/resources/backend_versions.json").read_text()
    )["llamacpp"]["vulkan"]
    with Lemond() as lemond:
        # Persist vulkan_bin=latest BEFORE forcing version.txt; the hot-swap
        # would otherwise overwrite our forced value.
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        time.sleep(0.5)
        installed_after_latest = (
            vulkan_version_txt().read_text(errors="replace").strip()
        )
        # Force version.txt back to the baseline (older than github latest)
        write_version_txt(vulkan_version_txt(), baseline)
        # Bust the system-info cache by triggering an install on a different
        # backend (cheap on Linux: cpu is small).
        install_backend("llamacpp", "cpu")
        state = backend_state("llamacpp", "vulkan")
        if state.get(
            "state"
        ) == "update_available" and "Newer upstream release" in state.get(
            "message", ""
        ):
            r.ok(
                f"F1 update_available fires (installed={baseline} < latest={installed_after_latest})"
            )
        else:
            r.fail(
                "F1 update_available",
                f"state={state.get('state')!r} message={state.get('message')!r}",
            )

    # F2 — force version.txt below baseline; expect update_required
    with Lemond() as lemond:
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        time.sleep(0.5)
        write_version_txt(vulkan_version_txt(), EXPLICIT_TAG_BELOW_BASELINE)
        install_backend("llamacpp", "cpu")  # bust cache
        state = backend_state("llamacpp", "vulkan")
        if state.get("state") == "update_required":
            r.ok(
                f"F2 update_required forced (installed={EXPLICIT_TAG_BELOW_BASELINE} < baseline={baseline})"
            )
        else:
            r.fail(
                "F2 update_required",
                f"state={state.get('state')!r} message={state.get('message')!r}",
            )

    # F3 — builtin pin with mismatched version.txt still emits update_required
    with Lemond() as lemond:
        set_config({"llamacpp": {"vulkan_bin": "builtin"}})
        time.sleep(0.5)
        write_version_txt(vulkan_version_txt(), EXPLICIT_TAG_BELOW_BASELINE)
        install_backend("llamacpp", "cpu")  # bust cache
        state = backend_state("llamacpp", "vulkan")
        if state.get("state") == "update_required":
            r.ok("F3 builtin + mismatched version.txt -> update_required (regression)")
        else:
            r.fail("F3 builtin regression", f"state={state.get('state')!r}")


def scenario_g1(r: Result):
    """G1: no_fetch_executables blocks latest with clear error."""
    print("\n=== G1: no_fetch_executables blocks latest ===")
    reset_config_to_defaults()
    reset_vulkan_install()
    with Lemond() as lemond:
        set_config({"no_fetch_executables": True})
        # Setting latest should still validate at config-set; the install fired
        # by hot-swap is what should be blocked. install_backend() throws
        # "Fetching executable artifacts is disabled" — handle_bin_change
        # catches and logs WARN.
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        time.sleep(0.5)
        log = lemond.log_text()
        if "Fetching executable artifacts is disabled" in log:
            r.ok("G1 no_fetch_executables blocks install attempt")
        else:
            r.fail(
                "G1 no_fetch_executables",
                "expected 'Fetching executable artifacts is disabled' in log",
            )


def scenario_g2(r: Result):
    """G2: offline=true falls back to version.txt for latest, hard-errors without it."""
    print("\n=== G2: offline interaction ===")
    reset_config_to_defaults()
    baseline = json.loads(
        (REPO / "src/cpp/resources/backend_versions.json").read_text()
    )["llamacpp"]["vulkan"]

    # Sub-case A: offline + latest + version.txt present -> reuse + WARN
    with Lemond() as lemond:
        # First install something so version.txt exists
        set_config({"llamacpp": {"vulkan_bin": "builtin"}})
        time.sleep(0.5)
        if not vulkan_version_txt().exists():
            install_backend("llamacpp", "vulkan")
        installed = vulkan_version_txt().read_text(errors="replace").strip()
        # Now go offline and switch to latest
        set_config({"offline": True, "llamacpp": {"vulkan_bin": "latest"}})
        time.sleep(0.5)
        log = lemond.log_text()
        if "offline: reusing installed" in log and installed in log:
            r.ok(f"G2a offline + latest reuses cached version.txt ({installed})")
        else:
            r.fail(
                "G2a offline reuse",
                f"expected 'offline: reusing installed' log line containing {installed}",
            )

    # Sub-case B: offline + latest + no version.txt -> hard error during install
    reset_vulkan_install()
    reset_config_to_defaults()
    with Lemond(env_extra={}) as lemond:
        set_config({"offline": True})
        # Setting latest with no install present -> install path should error.
        # handle_bin_change catches the error and logs WARN; the set_config
        # POST itself still succeeds.
        set_config({"llamacpp": {"vulkan_bin": "latest"}})
        time.sleep(0.5)
        log = lemond.log_text()
        if (
            "install_backend(llamacpp:vulkan) failed" in log
            and "no installed version" in log
        ):
            r.ok("G2b offline + no version.txt hard-errors gracefully")
        else:
            r.fail(
                "G2b offline no-cache",
                "expected install_backend failure with 'no installed version' in log",
            )


def scenario_h(r: Result):
    """H: regression — port/host/log_level callbacks still fire after sig change."""
    print("\n=== H: callback signature regression ===")
    reset_config_to_defaults()
    with Lemond() as lemond:
        # log_level change is the cleanest non-binding side-effect
        set_config({"log_level": "debug"})
        time.sleep(0.5)
        log = lemond.log_text()
        if "Log level changed to: debug" in log:
            r.ok("H log_level side-effect fires under new callback signature")
        else:
            r.fail("H log_level", "expected 'Log level changed to' log line missing")
        # Restore for cleanliness
        set_config({"log_level": "info"})


# --- Entry point --------------------------------------------------------------


def main():
    if not LEMOND.exists():
        print(f"lemond binary not found at {LEMOND}; build first.")
        return 2

    # Make sure no leftover lemond is running on PORT
    try:
        requests.get(f"{BASE}/health", timeout=1)
        print(
            f"\nA server is already running on port {PORT}. Stop it first; this "
            "test owns lemond's lifecycle."
        )
        return 2
    except requests.RequestException:
        pass

    r = Result()

    scenarios = [
        scenario_b1_b2_b3,
        scenario_c,
        scenario_e1,
        scenario_e2,
        scenario_f1_f2_f3,
        scenario_g1,
        scenario_g2,
        scenario_h,
        # D is the slowest (pulls + loads a model); run last so other failures
        # surface quickly.
        scenario_d,
    ]
    for fn in scenarios:
        try:
            fn(r)
        except Exception as e:
            r.fail(fn.__name__, f"unexpected exception: {e!r}")

    ok = r.summary()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
