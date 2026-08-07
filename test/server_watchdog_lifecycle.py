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
import threading
import time
import unittest

import requests

from utils.capabilities import (
    get_current_config,
    set_current_config,
    skip_if_unsupported,
)
from utils.server_base import ServerTestBase, run_server_tests
from utils.test_models import PORT, TIMEOUT_DEFAULT, TIMEOUT_MODEL_OPERATION

WATCHDOG_WAIT_SECONDS = int(os.environ.get("LEMONADE_TEST_WATCHDOG_WAIT_SECONDS", "60"))
POLL_SECONDS = float(
    os.environ.get("LEMONADE_TEST_WATCHDOG_ASSERT_POLL_SECONDS", "0.5")
)


def _headers():
    api_key = os.environ.get("LEMONADE_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


@unittest.skipIf(
    os.name == "nt", "POSIX zombie/reap assertion is not meaningful on Windows"
)
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
        self.assertGreater(
            entry.get("pid", 0), 0, f"/health did not expose a backend pid: {entry}"
        )
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
        self.fail(
            f"Timed out waiting for {model_name!r} to be loaded. Last health={last_health}"
        )

    def _wait_for_model_absent_from_health(
        self, model_name, timeout=WATCHDOG_WAIT_SECONDS
    ):
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

    def _non_streaming_chat_completion(self, model_name):
        response = requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": model_name,
                "messages": [
                    {
                        "role": "user",
                        "content": (
                            "Write five short numbered facts about reliable software. "
                            "Keep each fact under one sentence."
                        ),
                    }
                ],
                "max_tokens": 128,
                "stream": False,
            },
            headers=_headers(),
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200, response.text)
        payload = response.json()
        self.assertNotIn(
            "error",
            payload,
            "Non-streaming request should be replayed after backend restart, not returned as an error.",
        )
        self.assertTrue(
            payload.get("choices"),
            f"Non-streaming recovery returned no choices: {payload}",
        )
        return payload

    def _stream_chat_completion(self, model_name):
        response = requests.post(
            f"{self.base_url}/chat/completions",
            json={
                "model": model_name,
                "messages": [
                    {"role": "user", "content": "Say OK in one short sentence."}
                ],
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
        print(
            f"[OK] Streaming request reloaded {model_name}: pid {old_pid} -> {new_pid}"
        )

    @skip_if_unsupported("chat_completions")
    def test_active_non_streaming_request_reloads_and_replays_after_backend_crash(self):
        """An in-flight non-streaming request must survive backend restart.

        This is the user-visible recovery path for a hung/crashed backend: the
        current request may be delayed while the child process is reset, but the
        router must reload the same model and replay the request instead of
        returning a watchdog/reset error to the client.
        """
        model_name = self._test_model()

        loaded_before = self._load_model(model_name)
        old_pid = int(loaded_before["pid"])
        print(f"[SETUP] Loaded {model_name} with backend pid {old_pid}")

        result = {}

        def run_request():
            try:
                result["payload"] = self._non_streaming_chat_completion(model_name)
            except (
                Exception
            ) as exc:  # noqa: BLE001 - preserve assertion details across thread boundary
                result["error"] = exc

        worker = threading.Thread(target=run_request, daemon=True)
        worker.start()

        # Kill shortly after the request is issued. If the backend dies before it
        # receives the request, this still exercises the demand-driven reload path;
        # if it dies mid-request, it exercises the retry-after-reset path.
        time.sleep(
            float(
                os.environ.get(
                    "LEMONADE_TEST_WATCHDOG_ACTIVE_KILL_DELAY_SECONDS", "0.05"
                )
            )
        )
        self._kill_backend_process(old_pid)
        print(f"[TEST] Sent SIGKILL to active backend pid {old_pid}")

        worker.join(TIMEOUT_MODEL_OPERATION + WATCHDOG_WAIT_SECONDS)
        self.assertFalse(
            worker.is_alive(), "Recovered non-streaming request did not finish"
        )
        if "error" in result:
            raise result["error"]

        self._wait_for_pid_reaped(old_pid)
        new_pid = self._assert_fresh_backend_pid(model_name, old_pid)

        # Prove the reloaded model is not merely visible in /health but actually
        # usable for another completion after the recovered in-flight request.
        self._non_streaming_chat_completion(model_name)
        print(
            f"[OK] Active non-streaming request reloaded {model_name}: pid {old_pid} -> {new_pid}"
        )

    @skip_if_unsupported("chat_completions")
    def test_next_non_streaming_request_reaps_reloads_and_returns_completion(self):
        """A non-streaming request sent while the backend is down must wait for reload.

        This is the strongest user-facing invariant for non-streaming recovery:
        if the child backend is already gone when the request arrives, the
        caller must still receive a completion after Lemonade reloads the last
        active model.
        """
        model_name = self._test_model()

        loaded_before = self._load_model(model_name)
        old_pid = int(loaded_before["pid"])
        print(f"[SETUP] Loaded {model_name} with backend pid {old_pid}")

        self._kill_backend_process(old_pid)
        print(
            f"[TEST] Sent SIGKILL to backend pid {old_pid} before non-streaming request"
        )

        self._non_streaming_chat_completion(model_name)
        self._wait_for_pid_reaped(old_pid)
        new_pid = self._assert_fresh_backend_pid(model_name, old_pid)

        # A second call verifies the replacement backend remains reachable after
        # the transparent replay completed.
        self._non_streaming_chat_completion(model_name)
        print(
            f"[OK] Non-streaming downtime request reloaded {model_name}: pid {old_pid} -> {new_pid}"
        )

    @skip_if_unsupported("chat_completions")
    def test_concurrent_non_streaming_request_during_reload_also_completes(self):
        """A request arriving during another request's reload window must complete.

        The first request should trigger lazy reload after the backend crash. A
        second request issued immediately afterwards must wait behind that reload
        and use the freshly loaded model instead of seeing a stale tombstone or a
        model-not-loaded error.
        """
        model_name = self._test_model()

        loaded_before = self._load_model(model_name)
        old_pid = int(loaded_before["pid"])
        print(f"[SETUP] Loaded {model_name} with backend pid {old_pid}")

        self._kill_backend_process(old_pid)
        print(
            f"[TEST] Sent SIGKILL to backend pid {old_pid} before concurrent requests"
        )

        results = [{}, {}]

        def run_request(index):
            try:
                results[index]["payload"] = self._non_streaming_chat_completion(
                    model_name
                )
            except (
                Exception
            ) as exc:  # noqa: BLE001 - preserve assertion details across thread boundary
                results[index]["error"] = exc

        workers = [
            threading.Thread(target=run_request, args=(0,), daemon=True),
            threading.Thread(target=run_request, args=(1,), daemon=True),
        ]

        workers[0].start()
        time.sleep(
            float(
                os.environ.get(
                    "LEMONADE_TEST_WATCHDOG_CONCURRENT_REQUEST_GAP_SECONDS", "0.05"
                )
            )
        )
        workers[1].start()

        for worker in workers:
            worker.join(TIMEOUT_MODEL_OPERATION + WATCHDOG_WAIT_SECONDS)
            self.assertFalse(
                worker.is_alive(),
                "Concurrent recovered non-streaming request did not finish",
            )

        for result in results:
            if "error" in result:
                raise result["error"]
            self.assertTrue(result.get("payload", {}).get("choices"), result)

        self._wait_for_pid_reaped(old_pid)
        new_pid = self._assert_fresh_backend_pid(model_name, old_pid)
        print(
            f"[OK] Concurrent non-streaming reload preserved both requests: pid {old_pid} -> {new_pid}"
        )


if __name__ == "__main__":
    run_server_tests(
        WatchdogLifecycleTests,
        description="WATCHDOG BACKEND LIFECYCLE TESTS",
        modality="llm",
        default_wrapped_server="llamacpp",
    )
