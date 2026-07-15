"""
Endpoint tests for the generic server-side job engine.

These exercise the job lifecycle with no inference backend: read-only ops
(system_info / models) plus a `sleep` op used to make pause / interrupt /
persistence observable. Each test runs its own isolated `lemond` on a private
cache dir and port so the persistence-across-restart case can kill and relaunch
the server without disturbing anything else.

Usage:
    python server_jobs.py
    python server_jobs.py --lemond-binary /path/to/lemond
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import requests

PORT = 13401
HOST = "127.0.0.1"
BASE = f"http://{HOST}:{PORT}/api/v1"

# Tiny llama.cpp model shared with the endpoint suite; small enough to load fast.
TEST_MODEL = "Tiny-Test-Model-GGUF"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_LEMOND_BINARY = None


def find_lemond_binary():
    if _LEMOND_BINARY:
        return _LEMOND_BINARY
    env = os.environ.get("LEMOND_BINARY")
    if env:
        return env
    for candidate in ("build/lemond", "build-debug/lemond", "build-release/lemond"):
        path = os.path.join(REPO_ROOT, candidate)
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(
        "could not locate the lemond binary; build it or pass --lemond-binary"
    )


class JobEngineTests(unittest.TestCase):
    def setUp(self):
        self.cache_dir = tempfile.mkdtemp(prefix="lemonade-jobs-")
        self.proc = None
        self.start_server()

    def tearDown(self):
        self.stop_server()
        shutil.rmtree(self.cache_dir, ignore_errors=True)

    # ── server lifecycle ────────────────────────────────────────────────

    def start_server(self):
        binary = find_lemond_binary()
        self.proc = subprocess.Popen(
            [binary, self.cache_dir, "--port", str(PORT), "--host", HOST],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 40
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("lemond exited during startup")
            try:
                r = requests.get(f"{BASE}/health", timeout=2)
                if r.status_code == 200:
                    return
            except requests.RequestException:
                pass
            time.sleep(0.25)
        raise RuntimeError("lemond did not become healthy in time")

    def stop_server(self, hard=False):
        if not self.proc:
            return
        if self.proc.poll() is None:
            if hard:
                self.proc.send_signal(signal.SIGKILL)
            else:
                try:
                    requests.post(f"http://{HOST}:{PORT}/internal/shutdown", timeout=5)
                except requests.RequestException:
                    self.proc.terminate()
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.send_signal(signal.SIGKILL)
                self.proc.wait(timeout=10)
        self.proc = None

    # ── helpers ─────────────────────────────────────────────────────────

    def create_job(self, name, steps, inputs=None, expect=202):
        body = {"name": name, "definition": {"steps": steps}}
        if inputs is not None:
            body["inputs"] = inputs
        r = requests.post(f"{BASE}/jobs", json=body, timeout=10)
        self.assertEqual(r.status_code, expect, r.text)
        return r.json()

    def get_job(self, job_id):
        r = requests.get(f"{BASE}/jobs/{job_id}", timeout=10)
        return r

    def poll_status(self, job_id, targets, timeout=30):
        if isinstance(targets, str):
            targets = {targets}
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            r = self.get_job(job_id)
            self.assertEqual(r.status_code, 200, r.text)
            last = r.json()
            if last["status"] in targets:
                return last
            time.sleep(0.2)
        self.fail(f"job {job_id} did not reach {targets}; last={last}")

    def step_by_id(self, job, step_id):
        for s in job["steps"]:
            if s["id"] == step_id:
                return s
        self.fail(f"step {step_id} not present in job")

    def poll_cursor(self, job_id, target_cursor, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = self.get_job(job_id)
            self.assertEqual(r.status_code, 200, r.text)
            body = r.json()
            if body["cursor"] == target_cursor:
                return body
            if body["status"] in ("completed", "failed"):
                self.fail(
                    f"job {job_id} reached {body['status']} before cursor "
                    f"'{target_cursor}'"
                )
            time.sleep(0.1)
        self.fail(f"job {job_id} cursor did not reach '{target_cursor}'")

    # ── real-backend helpers ────────────────────────────────────────────

    def installed_llamacpp_backend(self):
        r = requests.get(f"{BASE}/system-info", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        backends = r.json().get("recipes", {}).get("llamacpp", {}).get("backends", {})
        for name, info in backends.items():
            if info.get("state") in ("installed", "ready"):
                return name
        return None

    def ensure_test_model(self):
        r = requests.post(
            f"{BASE}/pull",
            json={"model_name": TEST_MODEL},
            timeout=600,
            stream=True,
        )
        # Drain the streamed progress so the request completes.
        for _ in r.iter_lines():
            pass
        self.assertEqual(r.status_code, 200, "failed to pull the test model")

    def require_real_backend(self):
        backend = self.installed_llamacpp_backend()
        if not backend:
            self.skipTest("no installed llamacpp backend available")
        self.ensure_test_model()
        return backend

    # ── tests ───────────────────────────────────────────────────────────

    def test_system_info_job_completes(self):
        job = self.create_job("sysinfo", [{"id": "a", "op": "system_info"}])
        done = self.poll_status(job["id"], "completed")
        self.assertIn("a", done["context"])
        self.assertTrue(done["context"]["a"])
        self.assertEqual(self.step_by_id(done, "a")["status"], "completed")

    def test_invalid_graph_rejected(self):
        backward = [
            {"id": "a", "op": "system_info", "on_done": "a"},
        ]
        r = requests.post(
            f"{BASE}/jobs",
            json={"name": "bad", "definition": {"steps": backward}},
            timeout=10,
        )
        self.assertEqual(r.status_code, 400, r.text)
        self.assertIn("error", r.json())

        unknown = [{"id": "a", "op": "does_not_exist"}]
        r2 = requests.post(
            f"{BASE}/jobs",
            json={"name": "bad2", "definition": {"steps": unknown}},
            timeout=10,
        )
        self.assertEqual(r2.status_code, 400, r2.text)
        self.assertIn("unknown op", r2.json()["error"])

    def test_when_skip(self):
        steps = [
            {"id": "a", "op": "system_info"},
            {"id": "b", "op": "system_info", "when": "false"},
        ]
        job = self.create_job("skip", steps)
        done = self.poll_status(job["id"], "completed")
        self.assertEqual(self.step_by_id(done, "a")["status"], "completed")
        self.assertEqual(self.step_by_id(done, "b")["status"], "skipped")

    def test_branch_on_input(self):
        # step "a" branches to "c" when inputs.pick == 'b', skipping "b".
        steps = [
            {
                "id": "a",
                "op": "system_info",
                "branch": [{"when": "${inputs.pick}=='b'", "goto": "c"}],
            },
            {"id": "b", "op": "system_info"},
            {"id": "c", "op": "system_info"},
        ]
        job = self.create_job("branch", steps, inputs={"pick": "b"})
        done = self.poll_status(job["id"], "completed")
        self.assertEqual(self.step_by_id(done, "a")["status"], "completed")
        self.assertEqual(self.step_by_id(done, "b")["status"], "pending")
        self.assertEqual(self.step_by_id(done, "c")["status"], "completed")

    def test_on_fail_goto_recovery(self):
        steps = [
            {
                "id": "boom",
                "op": "models",
                "params": {"id": "definitely-not-a-real-model"},
                "on_fail": "recover",
            },
            {"id": "recover", "op": "system_info"},
        ]
        job = self.create_job("recover", steps)
        done = self.poll_status(job["id"], "completed")
        self.assertEqual(self.step_by_id(done, "boom")["status"], "failed")
        self.assertEqual(self.step_by_id(done, "recover")["status"], "completed")

    def test_pause_resume(self):
        steps = [{"id": "wait", "op": "sleep", "params": {"ms": 6000}}]
        job = self.create_job("pause", steps)
        jid = job["id"]
        self.poll_status(jid, "running", timeout=10)
        r = requests.post(f"{BASE}/jobs/{jid}/pause", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "paused", timeout=15)
        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "completed", timeout=20)

    def test_interrupt_resume(self):
        steps = [{"id": "wait", "op": "sleep", "params": {"ms": 8000}}]
        job = self.create_job("interrupt", steps)
        jid = job["id"]
        self.poll_status(jid, "running", timeout=10)
        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        stopped = self.poll_status(jid, "interrupted", timeout=15)
        self.assertEqual(self.step_by_id(stopped, "wait")["status"], "pending")
        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "completed", timeout=20)

    def test_delete_terminal_and_active(self):
        # terminal job removed cleanly
        job = self.create_job("del", [{"id": "a", "op": "system_info"}])
        jid = job["id"]
        self.poll_status(jid, "completed")
        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(self.get_job(jid).status_code, 404)

        # active (long sleep) job deleted via interrupt-then-remove
        job2 = self.create_job(
            "del2", [{"id": "w", "op": "sleep", "params": {"ms": 8000}}]
        )
        jid2 = job2["id"]
        self.poll_status(jid2, "running", timeout=10)
        r = requests.delete(f"{BASE}/jobs/{jid2}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(self.get_job(jid2).status_code, 404)

    def test_persistence_across_restart(self):
        steps = [{"id": "w", "op": "sleep", "params": {"ms": 8000}}]
        job = self.create_job("persist", steps)
        jid = job["id"]
        self.poll_status(jid, "running", timeout=10)

        # Hard-kill the server while the sleep step is in flight, then restart.
        self.stop_server(hard=True)
        self.start_server()

        recovered = self.get_job(jid)
        self.assertEqual(recovered.status_code, 200, recovered.text)
        body = recovered.json()
        self.assertEqual(body["status"], "interrupted")
        self.assertIn("server restarted", body.get("error", ""))
        self.assertEqual(self.step_by_id(body, "w")["status"], "pending")

        # Resume re-runs the pending step and the job runs to completion.
        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "completed", timeout=25)

    # ── Phase 3: exclusive ops + slot gate ──────────────────────────────

    def test_real_exclusive_job(self):
        backend = self.require_real_backend()
        steps = [
            {"id": "u0", "op": "unload"},
            {
                "id": "ld",
                "op": "load",
                "params": {
                    "model": TEST_MODEL,
                    "llamacpp_backend": backend,
                    "ctx_size": 2048,
                    "merge_args": False,
                    "save_options": False,
                },
            },
            {
                "id": "say",
                "op": "chat",
                "params": {
                    "model": TEST_MODEL,
                    "messages": [{"role": "user", "content": "Say hi in one word."}],
                    "temperature": 0,
                    "max_completion_tokens": 32,
                },
            },
            {"id": "u1", "op": "unload"},
        ]
        job = self.create_job("real-exclusive", steps)
        done = self.poll_status(job["id"], "completed", timeout=120)
        self.assertEqual(self.step_by_id(done, "ld")["status"], "completed")
        self.assertEqual(self.step_by_id(done, "say")["status"], "completed")
        self.assertEqual(done["context"]["ld"]["loaded"], True)
        self.assertEqual(done["context"]["ld"]["backend"], backend)

        chat_out = done["context"]["say"]
        timings = chat_out.get("timings", {})
        usage = chat_out.get("usage", {})
        self.assertTrue(
            "prompt_ms" in timings
            or "predicted_per_second" in timings
            or "total_tokens" in usage,
            f"chat output carried neither timings nor usage: keys={list(chat_out)}",
        )

    def test_queue_behind_exclusive_job(self):
        backend = self.require_real_backend()

        # Control: with no job running, a normal chat on a preloaded model is prompt.
        requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
            },
            timeout=120,
        )
        t0 = time.time()
        r = requests.post(
            f"{BASE}/chat/completions",
            json={
                "model": TEST_MODEL,
                "messages": [{"role": "user", "content": "hi"}],
                "temperature": 0,
                "max_completion_tokens": 8,
            },
            timeout=60,
        )
        control_latency = time.time() - t0
        self.assertEqual(r.status_code, 200, r.text)

        # Now hold the exclusive slot with a load + a several-second sleep (no
        # final unload, so the queued chat finds the model still loaded).
        hold_ms = 6000
        steps = [
            {
                "id": "ld",
                "op": "load",
                "params": {
                    "model": TEST_MODEL,
                    "llamacpp_backend": backend,
                    "ctx_size": 2048,
                },
            },
            {"id": "hold", "op": "sleep", "params": {"ms": hold_ms}},
        ]
        job = self.create_job("queue", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)  # load done, provably mid-exclusive

        t0 = time.time()
        r = requests.post(
            f"{BASE}/chat/completions",
            json={
                "model": TEST_MODEL,
                "messages": [{"role": "user", "content": "hi"}],
                "temperature": 0,
                "max_completion_tokens": 8,
            },
            timeout=60,
        )
        queued_latency = time.time() - t0
        self.assertEqual(r.status_code, 200, r.text)

        # The queued request must have been held behind the job: it returns only
        # after the slot is released, so its latency dwarfs the control call and
        # the job is finished by the time it comes back.
        self.assertGreater(
            queued_latency,
            max(2.0, control_latency * 5),
            f"queued chat was not held behind the job "
            f"(queued={queued_latency:.2f}s, control={control_latency:.2f}s)",
        )
        self.assertEqual(self.get_job(jid).json()["status"], "completed")
        print(f"\n[queue] control={control_latency:.3f}s queued={queued_latency:.3f}s")

    def test_interrupt_mid_job_cleans_up(self):
        backend = self.require_real_backend()
        steps = [
            {
                "id": "ld",
                "op": "load",
                "params": {
                    "model": TEST_MODEL,
                    "llamacpp_backend": backend,
                    "ctx_size": 2048,
                },
            },
            {"id": "hold", "op": "sleep", "params": {"ms": 15000}},
            {"id": "u", "op": "unload"},
        ]
        job = self.create_job("interrupt-mid", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        stopped = self.poll_status(jid, "interrupted", timeout=20)
        self.assertEqual(self.step_by_id(stopped, "hold")["status"], "pending")

        # Reconcile: an interrupted exclusive job unloads the model it left
        # resident (it had loaded TEST_MODEL before the sleep it was stopped in).
        deadline = time.time() + 10
        while time.time() < deadline:
            if (
                requests.get(f"{BASE}/health", timeout=5).json().get("model_loaded")
                is None
            ):
                break
            time.sleep(0.25)
        self.assertIsNone(
            requests.get(f"{BASE}/health", timeout=5).json().get("model_loaded"),
            "interrupt did not unload the resident model",
        )

        # The slot was released cleanly: a normal request goes through promptly
        # rather than deadlocking behind an abandoned exclusive hold.
        t0 = time.time()
        r = requests.post(
            f"{BASE}/chat/completions",
            json={
                "model": TEST_MODEL,
                "messages": [{"role": "user", "content": "hi"}],
                "temperature": 0,
                "max_completion_tokens": 8,
            },
            timeout=30,
        )
        self.assertLess(time.time() - t0, 10.0, "slot was not released on interrupt")

        # Resume re-runs the pending sleep and the job finishes.
        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "completed", timeout=40)

    # ── Phase 4: a bench-shaped sweep (the AutoOpt methodology end-to-end) ──

    def test_bench_shaped_sweep(self):
        backend = self.require_real_backend()

        def config(tag, args):
            return [
                {"id": f"u_{tag}0", "op": "unload"},
                {
                    "id": f"ld_{tag}",
                    "op": "load",
                    "params": {
                        "model": TEST_MODEL,
                        "llamacpp_backend": backend,
                        "ctx_size": 2048,
                        "llamacpp_args": args,
                        "merge_args": False,
                        "save_options": False,
                    },
                },
                {
                    "id": f"run_{tag}",
                    "op": "chat",
                    "params": {
                        "model": TEST_MODEL,
                        "messages": [{"role": "user", "content": "Count to five."}],
                        "temperature": 0,
                        "max_completion_tokens": 24,
                    },
                    "extract": {
                        f"{tag}_tps": "timings.predicted_per_second",
                        f"{tag}_ttft": "timings.prompt_ms",
                    },
                },
                {"id": f"u_{tag}1", "op": "unload"},
            ]

        steps = config("a", "") + config("b", "-b 256")
        steps += [
            {
                "id": "decide",
                "op": "system_info",
                "branch": [{"when": "${a_tps} >= ${b_tps}", "goto": "a_wins"}],
                "on_done": "b_wins",
            },
            {"id": "a_wins", "op": "sleep", "params": {"ms": 10}, "on_done": "done"},
            {"id": "b_wins", "op": "sleep", "params": {"ms": 10}},
            {"id": "done", "op": "sleep", "params": {"ms": 1}},
        ]

        job = self.create_job("bench-sweep", steps)
        result = self.poll_status(job["id"], "completed", timeout=180)
        ctx = result["context"]

        # Both configs were measured and their throughput extracted.
        self.assertIn("a_tps", ctx)
        self.assertIn("b_tps", ctx)
        self.assertGreater(ctx["a_tps"], 0)
        self.assertGreater(ctx["b_tps"], 0)

        # The branch fired on the measured metric: exactly one winner ran, and it
        # is consistent with the extracted tps values.
        a_ran = self.step_by_id(result, "a_wins")["status"] == "completed"
        b_ran = self.step_by_id(result, "b_wins")["status"] == "completed"
        self.assertNotEqual(a_ran, b_ran, "exactly one winner branch should run")
        if ctx["a_tps"] >= ctx["b_tps"]:
            self.assertTrue(a_ran, "a had >= tps but b_wins ran")
        else:
            self.assertTrue(b_ran, "b had > tps but a_wins ran")

        # No model left resident after the sweep's final unload.
        h = requests.get(f"http://{HOST}:{PORT}/api/v1/health", timeout=5).json()
        self.assertIsNone(h.get("model_loaded"))


def parse_args():
    global _LEMOND_BINARY
    parser = argparse.ArgumentParser(description="Job engine endpoint tests")
    parser.add_argument("--lemond-binary", type=str, default=None)
    args, remaining = parser.parse_known_args()
    _LEMOND_BINARY = args.lemond_binary
    return remaining


if __name__ == "__main__":
    remaining = parse_args()
    sys.argv = [sys.argv[0]] + remaining
    unittest.main(verbosity=2)
