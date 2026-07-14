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
