import os
import signal
import time
import unittest
import requests
from utils.capabilities import (
    get_current_config,
    set_current_config,
    skip_if_unsupported,
)
from utils.server_base import ServerTestBase, run_server_tests, pull_model_with_retry
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)

EVICTION_POLL_INTERVAL = 0.5
EVICTION_POLL_TIMEOUT = 10
WATCHDOG_WAIT_SECONDS = 30
POLL_SECONDS = 0.5


def _headers():
    admin_key = os.environ.get("LEMONADE_ADMIN_API_KEY", "")
    headers = {}
    if admin_key:
        headers["Authorization"] = f"Bearer {admin_key}"
    return headers


class PinningTests(ServerTestBase):
    """Integration tests for model pinning capabilities."""

    _model_pulled = False
    _model2_pulled = False
    MODEL2 = "Qwen3-0.6B-GGUF"

    @classmethod
    def setUpClass(cls):
        wrapped_server, backend, modality = get_current_config()
        if wrapped_server is None:
            set_current_config(
                os.environ.get("LEMONADE_TEST_WRAPPED_SERVER", "llamacpp"),
                os.environ.get("LEMONADE_TEST_BACKEND"),
                "llm",
            )
        super().setUpClass()
        if not cls._model_pulled:
            print(f"\n[SETUP] Ensuring {ENDPOINT_TEST_MODEL} is pulled...")
            pull_model_with_retry(ENDPOINT_TEST_MODEL)
            cls._model_pulled = True

        if not cls._model2_pulled:
            print(f"\n[SETUP] Ensuring {cls.MODEL2} is pulled...")
            pull_model_with_retry(cls.MODEL2)
            cls._model2_pulled = True

    def setUp(self):
        super().setUp()
        requests.post(f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT)
        requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json={
                "max_loaded_models": 2,
                "auto_evict": False,
                "auto_evict_threshold_pct": 0.85,
            },
            headers=_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

    def _get_loaded_model_info(self, model_name):
        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT).json()
        for model in health.get("all_models_loaded", []):
            if model["model_name"] == model_name:
                return model
        return None

    def _wait_for_model_status(
        self, model_name, target_statuses, timeout=EVICTION_POLL_TIMEOUT
    ):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            info = self._get_loaded_model_info(model_name)
            status = info.get("status") if info else None
            if status in target_statuses or (None in target_statuses and info is None):
                return info
            time.sleep(EVICTION_POLL_INTERVAL)
        return self._get_loaded_model_info(model_name)

    def _wait_for_loaded_model(self, model_name, timeout=WATCHDOG_WAIT_SECONDS):
        deadline = time.time() + timeout
        last_health = None
        while time.time() < deadline:
            last_health = requests.get(
                f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT
            ).json()
            for model in last_health.get("all_models_loaded", []):
                if model.get("model_name") == model_name and model.get("loaded", True):
                    return model
            time.sleep(POLL_SECONDS)
        self.fail(
            f"Timed out waiting for {model_name!r} to be loaded. Last health={last_health}"
        )

    def _simulate_vram_pressure(self, pct):
        response = requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/simulate-vram-pressure",
            json={"pct": pct},
            headers=_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code, 200, f"Simulate VRAM failed: {response.text}"
        )

    def test_pinning_lru_eviction_protection(self):
        """Pinned models are protected from LRU eviction under capacity slot limits."""
        # Set max loaded models to 1
        requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json={"max_loaded_models": 1},
            headers=_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        # 1. Load model 1 pinned
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": True},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # 2. Try loading model 2. Since max_loaded_models is 1 and model 1 is pinned, this must fail.
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": self.MODEL2},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 409)
        self.assertEqual(response.json()["error"]["code"], "slots_pinned_error")

        # 3. Unpin model 1
        response = requests.post(
            f"{self.internal_url}/pin",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": False},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        self.assertFalse(response.json().get("pinned"))

        # 4. Try loading model 2 again. Now it should succeed and evict model 1.
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": self.MODEL2},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # Verify model 1 is evicted and model 2 is loaded
        self.assertIsNone(self._get_loaded_model_info(ENDPOINT_TEST_MODEL))
        self.assertIsNotNone(self._get_loaded_model_info(self.MODEL2))

    def test_eviction_engine_skips_pinned_models(self):
        """The EvictionEngine ignores pinned models during idle and VRAM pressure checks."""
        requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json={"auto_evict": True, "auto_evict_threshold_pct": 0.90},
            headers=_headers(),
            timeout=TIMEOUT_DEFAULT,
        )

        # 1. Load model 1 (pinned)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": True},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        # Sleep briefly so ENDPOINT_TEST_MODEL is older
        time.sleep(2)

        # 2. Load model 2 (unpinned)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": self.MODEL2, "pinned": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertIsNotNone(self._get_loaded_model_info(ENDPOINT_TEST_MODEL))
        self.assertIsNotNone(self._get_loaded_model_info(self.MODEL2))

        # 3. Simulate VRAM pressure
        self._simulate_vram_pressure(0.95)

        # Model 2 (unpinned, newer) should be evicted because model 1 (pinned, older) is protected.
        info2 = self._wait_for_model_status(self.MODEL2, {None, "evicting", "unloaded"})
        evicted2 = info2 is None or info2.get("status") in ("evicting", "unloaded")
        self.assertTrue(
            evicted2, "Unpinned model should be evicted under VRAM pressure"
        )
        self.assertIsNotNone(
            self._get_loaded_model_info(ENDPOINT_TEST_MODEL),
            "Pinned model must remain loaded",
        )

    def test_pinned_model_bypasses_idle_timeout_downsize(self):
        """Pinned models are not downsized by the EvictionEngine idle timer."""
        requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": ENDPOINT_TEST_MODEL,
                "pinned": True,
                "auto_evict": True,
                "downsize_idle_timeout": 2,
                "evict_idle_timeout": 300,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertEqual(info.get("status"), "ready")

        # Wait to span the idle timeout check (which runs every 5s)
        time.sleep(8)

        # Pinned model should still be in 'ready' status, NOT 'downsized'
        info_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info_after)
        self.assertEqual(info_after.get("status"), "ready")

    @unittest.skipIf(os.name == "nt", "POSIX signal tests skipped on Windows")
    def test_watchdog_reload_preserves_pin_state(self):
        """Watchdog reloads preserve the pinning status of the crashed backend."""
        # 1. Load model pinned
        entry = self._get_loaded_model_info(self.MODEL2)
        if entry is None:
            response = requests.post(
                f"{self.base_url}/load",
                json={"model_name": self.MODEL2, "pinned": True},
                timeout=TIMEOUT_MODEL_OPERATION,
            )
            self.assertEqual(response.status_code, 200)
            entry = self._wait_for_loaded_model(self.MODEL2)
        else:
            # Pin it dynamically
            requests.post(
                f"{self.internal_url}/pin",
                json={"model_name": self.MODEL2, "pinned": True},
                timeout=TIMEOUT_DEFAULT,
            )
            entry = self._wait_for_loaded_model(self.MODEL2)

        self.assertTrue(entry.get("pinned"), f"Model must be pinned initially: {entry}")
        old_pid = int(entry["pid"])

        # 2. Kill backend process
        os.kill(old_pid, signal.SIGKILL)

        # 3. Request completion to trigger watchdog reload
        client = self.get_openai_client()
        completion = client.chat.completions.create(
            model=self.MODEL2,
            messages=[{"role": "user", "content": "Hi"}],
            max_tokens=8,
        )
        self.assertTrue(completion.choices)

        # 4. Check status and ensure fresh pid and retained pin
        entry_after = self._wait_for_loaded_model(self.MODEL2)
        new_pid = int(entry_after["pid"])
        self.assertNotEqual(old_pid, new_pid)
        self.assertTrue(
            entry_after.get("pinned"),
            "Pinned flag must be preserved after watchdog reload",
        )

    def test_pin_api_validation(self):
        """The /pin endpoint rejects invalid requests with HTTP 400 and structured JSON errors."""
        # 1. Invalid JSON
        response = requests.post(f"{self.internal_url}/pin", data="{invalid json")
        self.assertEqual(response.status_code, 400)
        self.assertIn("Invalid JSON body", response.json()["error"]["message"])
        self.assertEqual(response.json()["error"]["type"], "invalid_request_error")

        # 2. Missing model/model_name
        response = requests.post(f"{self.internal_url}/pin", json={"pinned": True})
        self.assertEqual(response.status_code, 400)
        self.assertIn(
            "Parameter 'model' or 'model_name' is required",
            response.json()["error"]["message"],
        )
        self.assertEqual(response.json()["error"]["type"], "invalid_request_error")

        # 3. Missing pinned
        response = requests.post(
            f"{self.internal_url}/pin", json={"model": ENDPOINT_TEST_MODEL}
        )
        self.assertEqual(response.status_code, 400)
        self.assertIn(
            "Parameter 'pinned' is required and must be a boolean",
            response.json()["error"]["message"],
        )
        self.assertEqual(response.json()["error"]["type"], "invalid_request_error")

        # 4. Non-boolean pinned
        response = requests.post(
            f"{self.internal_url}/pin",
            json={"model": ENDPOINT_TEST_MODEL, "pinned": "true"},
        )
        self.assertEqual(response.status_code, 400)
        self.assertIn(
            "Parameter 'pinned' is required and must be a boolean",
            response.json()["error"]["message"],
        )
        self.assertEqual(response.json()["error"]["type"], "invalid_request_error")

    def test_pinning_idempotency(self):
        """A subsequent load without explicit pin intent must preserve the pinned status of a model."""
        # 1. Load/pin model
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": True},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertTrue(info.get("pinned"), "Model should be pinned initially")

        # 2. Call load again without explicit pin intent (pinned omitted)
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # 3. Verify it remains pinned
        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertTrue(
            info.get("pinned"),
            "Model should remain pinned after load without pin intent",
        )

        # 4. Explicitly unpin (either via /internal/pin or load with pinned=False)
        # Let's test /internal/pin first
        response = requests.post(
            f"{self.internal_url}/pin",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": False},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)

        # Verify it becomes unpinned
        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertFalse(info.get("pinned"), "Model should be unpinned dynamically")

        # 5. Let's also test load with pinned=False explicitly unpins an already pinned model
        # Pin it again
        requests.post(
            f"{self.internal_url}/pin",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": True},
            timeout=TIMEOUT_DEFAULT,
        )
        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertTrue(info.get("pinned"))

        # Explicitly load with pinned=False
        response = requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "pinned": False},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(response.status_code, 200)

        # Verify it becomes unpinned
        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertFalse(
            info.get("pinned"),
            "Model should be unpinned by explicit load with pinned=False",
        )


if __name__ == "__main__":
    run_server_tests(
        PinningTests,
        description="MODEL PINNING CAPABILITY TESTS",
        modality="llm",
        default_wrapped_server="llamacpp",
    )
