"""
Regression test for watchdog backend lifecycle recovery.

The test expects a Lemonade server to already be running on PORT from
utils.test_models (13305 by default), matching the existing server_* tests.

Recommended local run for the PR branch:

    # In one terminal, start lemond from the patched build. Fast watchdog polling
    # makes this test finish quickly and avoids waiting for the default poll.
    LEMONADE_BACKEND_WATCHDOG_POLL_SECONDS=1 ./build/lemond

    # In another terminal, from the repo root:
    python test/server_watchdog_lifecycle.py --wrapped-server llamacpp --backend vulkan

Optional overrides:
    LEMONADE_TEST_MODEL=<model-name>                 # defaults to capability test model
    LEMONADE_TEST_WATCHDOG_WAIT_SECONDS=60           # cleanup/reload poll timeout
"""

import json
import os
import signal
import time
import unittest

import requests

from utils.capabilities import get_current_config, set_current_config, skip_if_unsupported
from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import PORT, TIMEOUT_DEFAULT, TIMEOUT_MODEL_OPERATION


WATCHDOG_WAIT_SECONDS = int(os.environ.get("LEMONADE_TEST_WATCHDOG_WAIT_SECONDS", "60"))
POLL_SECONDS = float(os.environ.get("LEMONADE_TEST_WATCHDOG_ASSERT_POLL_SECONDS", "0.5"))


def _headers():
    api_key = os.environ.get("LEMONADE_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


@unittest.skipIf(os.name == "nt", "POSIX zombie/reap assertion is not meaningful on Windows")
class WatchdogLifecycleTests(ServerTestBase):
    """Crash/reap/reload coverage for wrapped backend watchdog lifecycle."""

    @classmethod
    def setUpClass(cls):
        # Make the file usable under pytest too. The existing test harness calls
        # run_server_tests(), which sets this before setUpClass(); pytest does not.
        wrapped_server, backend, modality = get_current_config()
        if wrapped_server is None:
            set_current_config(
                os.environ.get("LEMONADE_TEST_WRAPPED_SERVER", "llamacpp"),
                os.environ.get("LEMONADE_TEST_BACKEND"),
                "llm",
            )
        super().setUpClass()

    def tearDown(self):
        # Best-effort cleanup so a failed run does not leave a backend around.
        try:
            requests.post(
                f"{self.base_url}/unload",
                json={},
                headers=_headers(),
                timeout=TIMEOUT_DEFAULT,
            )
        except Exception:
            pass
        super().tearDown()

    def _test_model(self):
        return os.environ.get("LEMONADE_TEST_MODEL") or self.get_test_model("llm")

    def _health(self):
        response = requests.get(
            f"{self.base_url}/health",
            headers=_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        return response.json()

    def _loaded_model_entry(self, model_name):
        for model in self._health().get("all_models_loaded", []):
            if model.get("model_name") == model_name:
                return model
        return None

    def _load_model(self, model_name):
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": model_name},
            headers=_headers(),
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        response.raise_for_status()
        entry = self._wait_for_loaded_model(model_name)
        self.assertGreater(entry.get("pid", 0), 0, f"/health did not expose a backend pid: {entry}")
        return entry

    def _wait_for_loaded_model(self, model_name, timeout=WATCHDOG_WAIT_SECONDS):
        deadline = time.time() + timeout
        last_health = None
        while time.time() < deadline:
            last_health = self._health()
            for model in last_health.get("all_models_loaded", []):
                if model.get("model_name") == model_name and model.get("loaded", True):
                    return model
            time.sleep(POLL_SECONDS)
        self.fail(f"Timed out waiting for {model_name!r} to be loaded. Last health={last_health}")

    def _wait_for_model_absent_from_health(self, model_name, timeout=WATCHDOG_WAIT_SECONDS):
        deadline = time.time() + timeout
        last_health = None
        while time.time() < deadline:
            last_health = self._health()
            loaded_models = last_health.get("all_models_loaded", [])
            if all(model.get("model_name") != model_name for model in loaded_models):
                return
            time.sleep(POLL_SECONDS)
        self.fail(
            f"{model_name!r} still appears in /health all_models_loaded after backend crash. "
            f"This leaves stale UI/client state. Last health={last_health}"
        )

    def _proc_state(self, pid):
        """Return Linux /proc state char, or None when the pid no longer exists."""
        stat_path = f"/proc/{pid}/stat"
        try:
            with open(stat_path, "r", encoding="utf-8") as stat_file:
                stat = stat_file.read().strip()
        except FileNotFoundError:
            return None

        # /proc/<pid>/stat has the comm field in parentheses and it may contain
        # spaces, so split only after the final closing parenthesis.
        after_comm = stat.rsplit(")", 1)[1].strip()
        return after_comm.split()[0] if after_comm else None

    def _wait_for_pid_reaped(self, pid, timeout=WATCHDOG_WAIT_SECONDS):
        deadline = time.time() + timeout
        last_state = None
        while time.time() < deadline:
            last_state = self._proc_state(pid)
            if last_state is None:
                return
            time.sleep(POLL_SECONDS)
        self.fail(
            f"Backend pid {pid} still exists after watchdog cleanup; "
            f"last /proc state={last_state!r}. State 'Z' means the child is still a zombie."
        )

    def _kill_backend_process(self, pid):
        # SIGKILL simulates a hard backend crash such as vk::DeviceLostError more
        # reliably than a graceful /unload path. The Lemonade parent must notice
        # this, reap the child, remove stale model state, and allow reload.
        os.kill(pid, signal.SIGKILL)

    def _stream_chat_completion(self, model_name):
        response = requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": model_name,
                "messages": [{"role": "user", "content": "Say OK in one short sentence."}],
                "max_tokens": 8,
                "stream": True,
            },
            headers=_headers(),
            timeout=TIMEOUT_MODEL_OPERATION,
            stream=True,
        )
        self.assertEqual(response.status_code, 200, response.text)

        saw_done = False
        saw_content_or_role = False
        errors = []

        for raw_line in response.iter_lines(decode_unicode=True):
            if not raw_line or not raw_line.startswith("data:"):
                continue
            payload = raw_line.split(":", 1)[1].strip()
            if payload == "[DONE]":
                saw_done = True
                break
            try:
                event = json.loads(payload)
            except json.JSONDecodeError:
                continue
            if "error" in event:
                errors.append(event["error"])
                continue
            for choice in event.get("choices", []):
                delta = choice.get("delta") or {}
                if delta.get("role") or delta.get("content"):
                    saw_content_or_role = True

        self.assertFalse(
            errors,
            "Streaming request returned an SSE error instead of reloading the crashed backend: "
            f"{errors}",
        )
        self.assertTrue(
            saw_done or saw_content_or_role,
            "Streaming request after backend crash did not produce a valid SSE completion.",
        )

    def _assert_fresh_backend_pid(self, model_name, old_pid):
        loaded_after = self._wait_for_loaded_model(model_name)
        new_pid = int(loaded_after["pid"])
        self.assertGreater(new_pid, 0, loaded_after)
        self.assertNotEqual(
            new_pid,
            old_pid,
            "Recovery should start a fresh backend process after the crashed one was reaped.",
        )
        return new_pid

    def test_idle_crashed_backend_is_reaped_and_removed_from_health(self):
        """A backend that dies while idle must not remain as loaded state.

        This specifically guards the reviewer report that the watchdog noticed
        the backend crash, but /health still made clients believe the model was
        loaded and the child remained as a zombie.
        """
        model_name = self._test_model()

        loaded_before = self._load_model(model_name)
        old_pid = int(loaded_before["pid"])
        print(f"[SETUP] Loaded {model_name} with backend pid {old_pid}")

        self._kill_backend_process(old_pid)
        print(f"[TEST] Sent SIGKILL to idle backend pid {old_pid}")

        self._wait_for_pid_reaped(old_pid)
        self._wait_for_model_absent_from_health(model_name)
        print(f"[OK] Backend pid {old_pid} was reaped and removed from /health")

    @skip_if_unsupported("chat_completions_streaming")
    def test_next_streaming_request_reaps_and_reloads_crashed_backend(self):
        """The next streaming request after a crash must reload transparently.

        This covers the demand-driven path where the client sends another
        request before waiting for the watchdog poll loop. It should not receive
        a backend_watchdog_reset SSE error when no partial stream was delivered.
        """
        model_name = self._test_model()

        loaded_before = self._load_model(model_name)
        old_pid = int(loaded_before["pid"])
        print(f"[SETUP] Loaded {model_name} with backend pid {old_pid}")

        self._kill_backend_process(old_pid)
        print(f"[TEST] Sent SIGKILL to backend pid {old_pid}")

        self._stream_chat_completion(model_name)
        self._wait_for_pid_reaped(old_pid)
        new_pid = self._assert_fresh_backend_pid(model_name, old_pid)
        print(f"[OK] Streaming request reloaded {model_name}: pid {old_pid} -> {new_pid}")


if __name__ == "__main__":
    run_server_tests(
        WatchdogLifecycleTests,
        description="WATCHDOG BACKEND LIFECYCLE TESTS",
        modality="llm",
        default_wrapped_server="llamacpp",
    )
