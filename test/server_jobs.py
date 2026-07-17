import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import requests

PORT = 13401
HOST = "127.0.0.1"
BASE = f"http://{HOST}:{PORT}/api/v1"

TEST_MODEL = "Tiny-Test-Model-GGUF"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_LEMOND_BINARY = None
_BACKEND_BIN_TEMPLATE = None
_BACKEND_INSTALL_ATTEMPTED = False


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
        if _BACKEND_BIN_TEMPLATE and os.path.isdir(_BACKEND_BIN_TEMPLATE):
            shutil.copytree(
                _BACKEND_BIN_TEMPLATE,
                os.path.join(self.cache_dir, "bin"),
                symlinks=True,
            )
        self.proc = None
        self.start_server()

    def tearDown(self):
        self.stop_server()
        shutil.rmtree(self.cache_dir, ignore_errors=True)

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

    def poll_gone(self, job_id, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.get_job(job_id).status_code == 404:
                return
            time.sleep(0.2)
        self.fail(f"job {job_id} was not erased in time")

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

        for _ in r.iter_lines():
            pass
        self.assertEqual(r.status_code, 200, "failed to pull the test model")

    def require_real_backend(self):
        global _BACKEND_BIN_TEMPLATE, _BACKEND_INSTALL_ATTEMPTED
        backend = self.installed_llamacpp_backend()
        if not backend and not _BACKEND_INSTALL_ATTEMPTED:
            _BACKEND_INSTALL_ATTEMPTED = True
            try:
                r = requests.post(
                    f"{BASE}/install",
                    json={"recipe": "llamacpp", "backend": "cpu", "subscribe": False},
                    timeout=900,
                )
            except requests.RequestException:
                r = None
            if r is not None and r.status_code == 200:
                backend = self.installed_llamacpp_backend()
                bin_dir = os.path.join(self.cache_dir, "bin")
                if backend and os.path.isdir(bin_dir):
                    _BACKEND_BIN_TEMPLATE = tempfile.mkdtemp(
                        prefix="lemonade-jobs-backend-"
                    )
                    shutil.rmtree(_BACKEND_BIN_TEMPLATE)
                    shutil.copytree(bin_dir, _BACKEND_BIN_TEMPLATE, symlinks=True)
        if not backend:
            self.skipTest("no installed llamacpp backend available")
        self.ensure_test_model()
        return backend

    def test_system_info_job_completes(self):
        job = self.create_job("sysinfo", [{"id": "a", "op": "system_info"}])
        done = self.poll_status(job["id"], "completed")
        self.assertIn("a", done["context"])
        self.assertTrue(done["context"]["a"])
        self.assertEqual(self.step_by_id(done, "a")["status"], "completed")

    def test_list_jobs_get(self):
        self.create_job("listme", [{"id": "a", "op": "system_info"}])
        r = requests.get(f"{BASE}/jobs", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        body = r.json()
        self.assertIn("jobs", body)
        self.assertIsInstance(body["jobs"], list)
        self.assertTrue(
            any(j.get("name") == "listme" for j in body["jobs"]),
            f"created job not present in list: {body}",
        )

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
        self.assertEqual(self.step_by_id(done, "b")["status"], "skipped")
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

        job = self.create_job("del", [{"id": "a", "op": "system_info"}])
        jid = job["id"]
        self.poll_status(jid, "completed")
        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(self.get_job(jid).status_code, 404)

        job2 = self.create_job(
            "del2", [{"id": "w", "op": "sleep", "params": {"ms": 8000}}]
        )
        jid2 = job2["id"]
        self.poll_status(jid2, "running", timeout=10)
        r = requests.delete(f"{BASE}/jobs/{jid2}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_gone(jid2)

    def test_persistence_across_restart(self):
        steps = [{"id": "w", "op": "sleep", "params": {"ms": 8000}}]
        job = self.create_job("persist", steps)
        jid = job["id"]
        self.poll_status(jid, "running", timeout=10)

        self.stop_server(hard=True)
        self.start_server()

        recovered = self.get_job(jid)
        self.assertEqual(recovered.status_code, 200, recovered.text)
        body = recovered.json()
        self.assertEqual(body["status"], "interrupted")
        self.assertIn("server restarted", body.get("error", ""))
        self.assertEqual(self.step_by_id(body, "w")["status"], "pending")

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "completed", timeout=25)

    def test_persistence_multiple_writes(self):
        ids = []
        for i in range(5):
            job = self.create_job(f"multi-{i}", [{"id": "a", "op": "system_info"}])
            jid = job["id"]
            self.poll_status(jid, "completed")
            ids.append(jid)

        jobs_path = os.path.join(self.cache_dir, "jobs.json")
        self.assertTrue(os.path.isfile(jobs_path), "jobs.json was not written")
        with open(jobs_path, "r", encoding="utf-8") as f:
            disk = json.load(f)
        self.assertEqual(disk.get("version"), 1)
        on_disk = {j["id"]: j for j in disk.get("jobs", [])}
        for jid in ids:
            self.assertIn(jid, on_disk, f"{jid} missing from persisted jobs.json")
            self.assertEqual(on_disk[jid]["status"], "completed")
            self.assertEqual(self.get_job(jid).json()["status"], on_disk[jid]["status"])

        recreated = self.create_job("multi-final", [{"id": "a", "op": "system_info"}])
        self.poll_status(recreated["id"], "completed")
        with open(jobs_path, "r", encoding="utf-8") as f:
            disk2 = json.load(f)
        ids_on_disk = {j["id"] for j in disk2.get("jobs", [])}
        self.assertIn(recreated["id"], ids_on_disk)
        for jid in ids:
            self.assertIn(jid, ids_on_disk, f"{jid} lost after further persists")

    def test_delete_active_is_durable_across_crash(self):
        steps = [{"id": "w", "op": "sleep", "params": {"ms": 30000}}]
        job = self.create_job("del-crash", steps)
        jid = job["id"]
        self.poll_status(jid, "running", timeout=10)

        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)

        with open(
            os.path.join(self.cache_dir, "jobs.json"), "r", encoding="utf-8"
        ) as f:
            on_disk = {j["id"]: j for j in json.load(f).get("jobs", [])}
        self.assertTrue(
            on_disk.get(jid, {}).get("deleted", jid not in on_disk),
            "the tombstone must be on disk before the DELETE ack "
            f"(entry: {on_disk.get(jid)})",
        )
        self.assertEqual(
            self.get_job(jid).status_code,
            404,
            "a deleted active job must be invisible immediately after the ack",
        )

        self.stop_server(hard=True)
        self.start_server()

        self.assertEqual(
            self.get_job(jid).status_code,
            404,
            "a crash after the DELETE ack must not resurrect the job",
        )
        listed = requests.get(f"{BASE}/jobs", timeout=10).json()["jobs"]
        self.assertNotIn(jid, [j["id"] for j in listed])

    def test_pause_and_interrupt_queued_jobs_take_effect_immediately(self):
        blocker = self.create_job(
            "blocker", [{"id": "w", "op": "sleep", "params": {"ms": 20000}}]
        )
        self.poll_status(blocker["id"], "running", timeout=10)

        queued_a = self.create_job(
            "queued-pause", [{"id": "w", "op": "sleep", "params": {"ms": 100}}]
        )
        queued_b = self.create_job(
            "queued-interrupt", [{"id": "w", "op": "sleep", "params": {"ms": 100}}]
        )
        self.assertEqual(self.get_job(queued_a["id"]).json()["status"], "queued")
        self.assertEqual(self.get_job(queued_b["id"]).json()["status"], "queued")

        r = requests.post(f"{BASE}/jobs/{queued_a['id']}/pause", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(
            self.get_job(queued_a["id"]).json()["status"],
            "paused",
            "pausing a queued job must take effect immediately",
        )

        r = requests.post(f"{BASE}/jobs/{queued_b['id']}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(
            self.get_job(queued_b["id"]).json()["status"],
            "interrupted",
            "interrupting a queued job must take effect immediately",
        )

        r = requests.post(f"{BASE}/jobs/{queued_a['id']}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertIn(
            self.get_job(queued_a["id"]).json()["status"], ("queued", "running")
        )

        r = requests.post(f"{BASE}/jobs/{blocker['id']}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(queued_a["id"], "completed", timeout=20)

    def test_job_cap_rejected_when_nothing_evictable(self):
        blocker = self.create_job(
            "cap-blocker", [{"id": "w", "op": "sleep", "params": {"ms": 60000}}]
        )
        self.poll_status(blocker["id"], "running", timeout=10)
        for i in range(49):
            self.create_job(
                f"cap-{i}", [{"id": "w", "op": "sleep", "params": {"ms": 60000}}]
            )

        rejected = self.create_job(
            "cap-overflow",
            [{"id": "w", "op": "sleep", "params": {"ms": 60000}}],
            expect=429,
        )
        self.assertIn("job limit", rejected.get("error", ""))

        listed = requests.get(f"{BASE}/jobs", timeout=10).json()["jobs"]
        victim = next(j["id"] for j in listed if j["status"] == "queued")
        r = requests.delete(f"{BASE}/jobs/{victim}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)

        self.create_job(
            "cap-after-delete", [{"id": "w", "op": "sleep", "params": {"ms": 100}}]
        )
        r = requests.post(f"{BASE}/jobs/{blocker['id']}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)

    def test_progress_counts_handled_failures(self):
        steps = [
            {
                "id": "boom",
                "op": "models",
                "params": {"id": "definitely-not-a-real-model"},
                "on_fail": "recover",
            },
            {"id": "recover", "op": "system_info"},
        ]
        job = self.create_job("progress-recovery", steps)
        done = self.poll_status(job["id"], "completed")
        self.assertEqual(self.step_by_id(done, "boom")["status"], "failed")

        listed = requests.get(f"{BASE}/jobs", timeout=10).json()["jobs"]
        summary = next(j for j in listed if j["id"] == job["id"])
        self.assertEqual(
            summary["progress"]["completed"],
            summary["progress"]["step_count"],
            "a completed recovery job must report full progress "
            "(handled failures count as processed steps)",
        )

        steps2 = [
            {
                "id": "boom",
                "op": "models",
                "params": {"id": "definitely-not-a-real-model"},
                "on_fail": "continue",
            },
            {"id": "after", "op": "system_info"},
        ]
        job2 = self.create_job("progress-continue", steps2)
        self.poll_status(job2["id"], "completed")
        listed = requests.get(f"{BASE}/jobs", timeout=10).json()["jobs"]
        summary2 = next(j for j in listed if j["id"] == job2["id"])
        self.assertEqual(
            summary2["progress"]["completed"], summary2["progress"]["step_count"]
        )

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
        self.poll_cursor(jid, "hold", timeout=60)

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

        self.assertGreater(
            queued_latency,
            max(2.0, control_latency * 5),
            f"queued chat was not held behind the job "
            f"(queued={queued_latency:.2f}s, control={control_latency:.2f}s)",
        )
        self.assertEqual(self.get_job(jid).json()["status"], "completed")
        print(f"\n[queue] control={control_latency:.3f}s queued={queued_latency:.3f}s")

    def test_tokenize_gated_behind_exclusive_job(self):
        backend = self.require_real_backend()
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
            f"{BASE}/tokenize",
            json={"model": TEST_MODEL, "content": "hello world"},
            timeout=30,
        )
        control_latency = time.time() - t0
        self.assertEqual(r.status_code, 200, r.text)

        hold_ms = 5000
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
        job = self.create_job("gate-tokenize", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        t0 = time.time()
        r = requests.post(
            f"{BASE}/tokenize",
            json={"model": TEST_MODEL, "content": "hello world"},
            timeout=60,
        )
        gated_latency = time.time() - t0
        self.assertEqual(r.status_code, 200, r.text)
        self.assertGreater(
            gated_latency,
            max(2.0, control_latency * 5),
            f"tokenize was not held behind the exclusive job "
            f"(gated={gated_latency:.2f}s, control={control_latency:.2f}s)",
        )
        self.assertEqual(self.get_job(jid).json()["status"], "completed")

    def test_exclusive_job_drains_inflight_chat(self):
        backend = self.require_real_backend()
        requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
            },
            timeout=120,
        )

        result = {}

        def long_chat():
            t0 = time.time()
            r = requests.post(
                f"{BASE}/chat/completions",
                json={
                    "model": TEST_MODEL,
                    "messages": [
                        {
                            "role": "user",
                            "content": "Write a very long story about a dragon.",
                        }
                    ],
                    "temperature": 0,
                    "max_completion_tokens": 1800,
                    "ignore_eos": True,
                },
                timeout=120,
            )
            result["status"] = r.status_code
            result["end_time"] = time.time()
            result["elapsed"] = result["end_time"] - t0

        chat_thread = threading.Thread(target=long_chat)
        chat_thread.start()
        time.sleep(0.3)

        job = self.create_job(
            "drain",
            [
                {
                    "id": "ld",
                    "op": "load",
                    "params": {
                        "model": TEST_MODEL,
                        "llamacpp_backend": backend,
                        "ctx_size": 2048,
                    },
                }
            ],
        )
        jid = job["id"]

        overlap_at = None
        while chat_thread.is_alive():
            status = self.get_job(jid).json()["status"]
            if status == "completed" and chat_thread.is_alive():
                overlap_at = time.time()
                break
            time.sleep(0.05)
        chat_thread.join()

        self.assertEqual(
            result.get("status"), 200, "the in-flight chat must not be disturbed"
        )
        if result.get("elapsed", 0) < 1.0:
            self.skipTest("chat finished too quickly to observe the drain window")
        overlap = (
            overlap_at is not None
            and result.get("end_time", overlap_at) - overlap_at > 0.5
        )
        self.assertFalse(
            overlap,
            "the exclusive job completed while an in-flight chat was still running "
            "(begin_exclusive did not drain in-flight requests)",
        )
        self.poll_status(jid, "completed", timeout=60)

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

        deadline = time.time() + 10
        while time.time() < deadline:
            if (
                requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded")
                is None
            ):
                break
            time.sleep(0.25)
        self.assertIsNone(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            "interrupt did not unload the resident model",
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
            timeout=30,
        )
        self.assertLess(time.time() - t0, 10.0, "slot was not released on interrupt")

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "completed", timeout=40)

    def test_delete_active_exclusive_cleans_up(self):
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
        job = self.create_job("delete-active-exclusive", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)

        deadline = time.time() + 15
        while time.time() < deadline:
            if (
                requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded")
                is None
            ):
                break
            time.sleep(0.25)
        self.assertIsNone(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            "deleting an active exclusive job did not unload the resident model",
        )
        self.poll_gone(jid)

    def test_preexisting_model_survives_job_interrupt(self):
        backend = self.require_real_backend()
        r = requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
            },
            timeout=120,
        )
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        steps = [
            {"id": "hold", "op": "sleep", "params": {"ms": 15000}},
            {"id": "u", "op": "unload"},
        ]
        job = self.create_job("preexisting-survives", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=30)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)

        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "interrupt cleanup unloaded a model the job did not load",
        )

    def _pinned_load_steps(self, backend):
        return [
            {"id": "u0", "op": "unload"},
            {
                "id": "ld",
                "op": "load",
                "params": {
                    "model": TEST_MODEL,
                    "llamacpp_backend": backend,
                    "ctx_size": 2048,
                    "pinned": True,
                },
            },
            {"id": "hold", "op": "sleep", "params": {"ms": 15000}},
            {"id": "u", "op": "unload"},
        ]

    def _wait_model_unloaded(self, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if (
                requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded")
                is None
            ):
                return
            time.sleep(0.25)

    def test_delete_active_pinned_model_cleans_up(self):
        backend = self.require_real_backend()
        job = self.create_job("delete-active-pinned", self._pinned_load_steps(backend))
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)

        self._wait_model_unloaded()
        self.assertIsNone(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            "deleting an active job did not clean up a model it pinned",
        )
        self.poll_gone(jid)

    def test_interrupt_pinned_model_cleans_up(self):
        backend = self.require_real_backend()
        job = self.create_job("interrupt-pinned", self._pinned_load_steps(backend))
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)

        self._wait_model_unloaded()
        self.assertIsNone(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            "interrupt did not clean up a model the job pinned",
        )

    def test_preexisting_pinned_model_survives_job_interrupt(self):
        backend = self.require_real_backend()
        r = requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
                "pinned": True,
            },
            timeout=120,
        )
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        steps = [
            {"id": "hold", "op": "sleep", "params": {"ms": 15000}},
            {"id": "u", "op": "unload"},
        ]
        job = self.create_job("preexisting-pinned-survives", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=30)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)

        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "interrupt cleanup unloaded a model pinned before the job started",
        )

    def loaded_model_entry(self, model):
        health = requests.get(f"{BASE}/health", timeout=15).json()
        for entry in health.get("all_models_loaded", []):
            if entry.get("model") == model or entry.get("model_name") == model:
                return entry
        self.fail(f"{model} not present in all_models_loaded: {health}")

    def loaded_model_pinned_state(self, model):
        return self.loaded_model_entry(model).get("pinned")

    def test_pin_state_of_preexisting_model_restored_after_job(self):
        backend = self.require_real_backend()
        r = requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
                "pinned": True,
            },
            timeout=120,
        )
        self.assertEqual(r.status_code, 200, r.text)
        self.assertTrue(self.loaded_model_pinned_state(TEST_MODEL))
        pid_before = self.loaded_model_entry(TEST_MODEL).get("pid")
        self.assertTrue(pid_before)

        steps = [
            {
                "id": "unpin",
                "op": "load",
                "params": {
                    "model": TEST_MODEL,
                    "llamacpp_backend": backend,
                    "ctx_size": 2048,
                    "pinned": False,
                },
            },
            {"id": "hold", "op": "sleep", "params": {"ms": 15000}},
        ]
        job = self.create_job("unpin-then-interrupt", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)
        mid = self.loaded_model_entry(TEST_MODEL)
        self.assertFalse(
            mid.get("pinned"),
            "the job load did not change the surviving model's pin state",
        )
        self.assertEqual(
            mid.get("pid"),
            pid_before,
            "a pin-only load reloaded the model instead of updating the pin in place",
        )
        self.assertFalse(
            mid.get("recipe_options", {}).get("pinned"),
            "the stored recipe options disagree with the pin flag after an "
            "in-place pin update",
        )

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)

        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "reconcile evicted a pre-job model",
        )
        after = self.loaded_model_entry(TEST_MODEL)
        self.assertTrue(
            after.get("pinned"),
            "reconcile did not restore the pre-job pin state of a surviving model",
        )
        self.assertEqual(
            after.get("pid"),
            pid_before,
            "the pre-job model did not actually survive the job (it was reloaded)",
        )

    def test_job_ownership_survives_pause_resume_interrupt(self):
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
            {"id": "hold", "op": "sleep", "params": {"ms": 4000}},
            {"id": "hold2", "op": "sleep", "params": {"ms": 20000}},
        ]
        job = self.create_job("pause-resume-interrupt", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        r = requests.post(f"{BASE}/jobs/{jid}/pause", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "paused", timeout=15)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "pause must keep the job's models resident for the resume",
        )

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_cursor(jid, "hold2", timeout=30)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)
        self.assertNotEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "a pause/resume cycle re-baselined the snapshot: the interrupt "
            "preserved a model the job itself introduced",
        )

    def test_delete_while_paused_cleans_up(self):
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
            {"id": "hold", "op": "sleep", "params": {"ms": 4000}},
            {"id": "hold2", "op": "sleep", "params": {"ms": 20000}},
        ]
        job = self.create_job("delete-while-paused", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/pause", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "paused", timeout=15)
        self.assertEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(self.get_job(jid).status_code, 404)
        self.poll_gone(jid)

        deadline = time.time() + 20
        while time.time() < deadline:
            loaded = (
                requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded")
            )
            if loaded != TEST_MODEL:
                break
            time.sleep(0.2)
        self.assertNotEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "deleting a paused job skipped reconciliation and leaked the "
            "model the job introduced",
        )

    def test_interrupted_job_restores_model_on_resume(self):
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
            {"id": "hold", "op": "sleep", "params": {"ms": 8000}},
            {
                "id": "say",
                "op": "chat",
                "params": {
                    "model": TEST_MODEL,
                    "messages": [{"role": "user", "content": "Say hi in one word."}],
                    "temperature": 0,
                    "max_completion_tokens": 16,
                },
            },
        ]
        job = self.create_job("interrupt-resume-restore", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)
        deadline = time.time() + 20
        while time.time() < deadline:
            loaded = (
                requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded")
            )
            if loaded != TEST_MODEL:
                break
            time.sleep(0.2)
        self.assertNotEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
            "interrupt cleanup did not unload the job-loaded model",
        )

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        done = self.poll_status(jid, "completed", timeout=90)
        self.assertEqual(
            self.step_by_id(done, "say")["status"],
            "completed",
            "resume after interrupt cleanup could not run the chat step "
            "(job-owned model was not restored)",
        )

    def test_external_model_survives_job_cleanup(self):
        backend = self.require_real_backend()
        self.stop_server()
        with open(
            os.path.join(self.cache_dir, "config.json"), "w", encoding="utf-8"
        ) as f:
            json.dump({"max_loaded_models": 2}, f)
        self.start_server()

        info = requests.get(f"{BASE}/models/{TEST_MODEL}", timeout=10).json()
        checkpoint = info.get("checkpoint")
        if not checkpoint:
            self.skipTest(f"cannot determine checkpoint of {TEST_MODEL}")
        clone = "user.TinyClone-Jobs"
        r = requests.post(
            f"{BASE}/pull",
            json={
                "model_name": clone,
                "checkpoint": checkpoint,
                "recipe": "llamacpp",
                "stream": False,
            },
            timeout=600,
        )
        self.assertEqual(r.status_code, 200, r.text)

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
            {"id": "hold", "op": "sleep", "params": {"ms": 4000}},
            {"id": "hold2", "op": "sleep", "params": {"ms": 30000}},
        ]
        job = self.create_job("external-survives", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/pause", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "paused", timeout=15)

        r = requests.post(
            f"{BASE}/load",
            json={
                "model_name": clone,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
            },
            timeout=120,
        )
        self.assertEqual(r.status_code, 200, r.text)
        clone_public = clone.split("user.", 1)[1]
        loaded = {
            e.get("model_name")
            for e in requests.get(f"{BASE}/health", timeout=15)
            .json()
            .get("all_models_loaded", [])
        }
        self.assertIn(TEST_MODEL, loaded)
        self.assertTrue({clone, clone_public} & loaded, f"clone not loaded: {loaded}")

        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_gone(jid)

        deadline = time.time() + 20
        while time.time() < deadline:
            loaded = {
                e.get("model_name")
                for e in requests.get(f"{BASE}/health", timeout=15)
                .json()
                .get("all_models_loaded", [])
            }
            if TEST_MODEL not in loaded:
                break
            time.sleep(0.2)
        self.assertNotIn(
            TEST_MODEL,
            loaded,
            "job cleanup did not unload the model the job introduced",
        )
        self.assertTrue(
            {clone, clone_public} & loaded,
            "job cleanup unloaded a model loaded externally while the job "
            "was paused",
        )

    def test_external_same_name_model_survives_delete_of_interrupted_job(self):
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
            {"id": "hold", "op": "sleep", "params": {"ms": 30000}},
        ]
        job = self.create_job("stale-ownership", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)
        deadline = time.time() + 20
        while time.time() < deadline:
            health = requests.get(f"{BASE}/health", timeout=15).json()
            if health.get("model_loaded") != TEST_MODEL:
                break
            time.sleep(0.2)

        r = requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 2048,
            },
            timeout=120,
        )
        self.assertEqual(r.status_code, 200, r.text)
        external_pid = self.loaded_model_entry(TEST_MODEL).get("pid")
        self.assertTrue(external_pid)

        r = requests.delete(f"{BASE}/jobs/{jid}", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_gone(jid)
        time.sleep(1.0)

        after = self.loaded_model_entry(TEST_MODEL)
        self.assertEqual(
            after.get("pid"),
            external_pid,
            "deleting an interrupted job unloaded an EXTERNAL residency of a "
            "model name the job used to own",
        )

    def test_resume_adopts_external_same_name_model(self):
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
            {"id": "hold", "op": "sleep", "params": {"ms": 8000}},
            {
                "id": "say",
                "op": "chat",
                "params": {
                    "model": TEST_MODEL,
                    "messages": [{"role": "user", "content": "Say hi in one word."}],
                    "temperature": 0,
                    "max_completion_tokens": 16,
                },
            },
        ]
        job = self.create_job("resume-adopts-external", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)
        deadline = time.time() + 20
        while time.time() < deadline:
            health = requests.get(f"{BASE}/health", timeout=15).json()
            if health.get("model_loaded") != TEST_MODEL:
                break
            time.sleep(0.2)

        r = requests.post(
            f"{BASE}/load",
            json={
                "model_name": TEST_MODEL,
                "llamacpp_backend": backend,
                "ctx_size": 4096,
            },
            timeout=120,
        )
        self.assertEqual(r.status_code, 200, r.text)
        external_pid = self.loaded_model_entry(TEST_MODEL).get("pid")

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        done = self.poll_status(jid, "completed", timeout=90)
        self.assertEqual(self.step_by_id(done, "say")["status"], "completed")
        self.assertEqual(
            self.loaded_model_entry(TEST_MODEL).get("pid"),
            external_pid,
            "resume restore replaced an external residency of the same model "
            "name instead of adopting it",
        )

    def test_alias_loaded_model_unloads_after_interrupt_resume_interrupt(self):
        backend = self.require_real_backend()
        info = requests.get(f"{BASE}/models/{TEST_MODEL}", timeout=10).json()
        checkpoint = info.get("checkpoint")
        if not checkpoint:
            self.skipTest(f"cannot determine checkpoint of {TEST_MODEL}")
        clone = "user.AliasClone-Jobs"
        alias = "AliasClone-Jobs"
        r = requests.post(
            f"{BASE}/pull",
            json={
                "model_name": clone,
                "checkpoint": checkpoint,
                "recipe": "llamacpp",
                "stream": False,
            },
            timeout=600,
        )
        self.assertEqual(r.status_code, 200, r.text)

        steps = [
            {
                "id": "ld",
                "op": "load",
                "params": {
                    "model": alias,
                    "llamacpp_backend": backend,
                    "ctx_size": 2048,
                },
            },
            {"id": "hold", "op": "sleep", "params": {"ms": 8000}},
            {"id": "hold2", "op": "sleep", "params": {"ms": 8000}},
        ]
        job = self.create_job("alias-ownership", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        def loaded_names():
            return {
                e.get("model_name")
                for e in requests.get(f"{BASE}/health", timeout=15)
                .json()
                .get("all_models_loaded", [])
            }

        def poll_unloaded():
            deadline = time.time() + 20
            while time.time() < deadline:
                if not ({alias, clone} & loaded_names()):
                    return True
                time.sleep(0.2)
            return False

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)
        self.assertTrue(
            poll_unloaded(), "first interrupt did not unload the alias-loaded model"
        )

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_cursor(jid, "hold", timeout=60)

        r = requests.post(f"{BASE}/jobs/{jid}/interrupt", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        self.poll_status(jid, "interrupted", timeout=20)
        self.assertTrue(
            poll_unloaded(),
            "interrupt after a resume left the alias-loaded model resident "
            "(stale alias-keyed ownership shadowed the live instance)",
        )

    def test_model_dependent_resume_after_hard_restart(self):
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
            {"id": "hold", "op": "sleep", "params": {"ms": 30000}},
            {
                "id": "say",
                "op": "chat",
                "params": {
                    "model": TEST_MODEL,
                    "messages": [{"role": "user", "content": "Say hi in one word."}],
                    "temperature": 0,
                    "max_completion_tokens": 16,
                },
            },
        ]
        job = self.create_job("restart-restore", steps)
        jid = job["id"]
        self.poll_cursor(jid, "hold", timeout=60)

        self.stop_server(hard=True)
        self.start_server()

        recovered = self.get_job(jid)
        self.assertEqual(recovered.status_code, 200, recovered.text)
        self.assertEqual(recovered.json()["status"], "interrupted")
        self.assertNotEqual(
            requests.get(f"{BASE}/health", timeout=15).json().get("model_loaded"),
            TEST_MODEL,
        )

        r = requests.post(f"{BASE}/jobs/{jid}/resume", timeout=10)
        self.assertEqual(r.status_code, 200, r.text)
        done = self.poll_status(jid, "completed", timeout=120)
        self.assertEqual(
            self.step_by_id(done, "say")["status"],
            "completed",
            "resume after a hard restart could not run the model-dependent "
            "step (job model state was not reconstructed)",
        )

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

        self.assertIn("a_tps", ctx)
        self.assertIn("b_tps", ctx)
        self.assertGreater(ctx["a_tps"], 0)
        self.assertGreater(ctx["b_tps"], 0)

        a_ran = self.step_by_id(result, "a_wins")["status"] == "completed"
        b_ran = self.step_by_id(result, "b_wins")["status"] == "completed"
        self.assertNotEqual(a_ran, b_ran, "exactly one winner branch should run")
        if ctx["a_tps"] >= ctx["b_tps"]:
            self.assertTrue(a_ran, "a had >= tps but b_wins ran")
        else:
            self.assertTrue(b_ran, "b had > tps but a_wins ran")

        h = requests.get(f"http://{HOST}:{PORT}/api/v1/health", timeout=15).json()
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
