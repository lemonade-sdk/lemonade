import os
import threading
import unittest
import requests
import time
from utils.server_base import ServerTestBase, pull_model_with_retry
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    SECOND_TEST_MODEL_EVICTION,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)

EVICTION_POLL_INTERVAL = 0.5
EVICTION_POLL_TIMEOUT = 10


class EvictionTests(ServerTestBase):
    """Tests for dynamic VRAM eviction engine."""

    _model_pulled = False
    _model2_pulled = False
    MODEL2 = SECOND_TEST_MODEL_EVICTION

    @classmethod
    def setUpClass(cls):
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

    def _get_loaded_model_info(self, model_name):
        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT).json()
        for model in health.get("all_models_loaded", []):
            if model["model_name"] == model_name:
                return model
        return None

    def _wait_for_model_status(
        self, model_name, target_statuses, timeout=EVICTION_POLL_TIMEOUT
    ):
        """Poll until the model reaches one of the target statuses, or timeout."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            info = self._get_loaded_model_info(model_name)
            status = info.get("status") if info else None
            if status in target_statuses or (None in target_statuses and info is None):
                return info
            time.sleep(EVICTION_POLL_INTERVAL)
        return self._get_loaded_model_info(model_name)

    def _simulate_vram_pressure(self, pct):
        admin_key = os.getenv("LEMONADE_ADMIN_API_KEY", "")
        headers = {}
        if admin_key:
            headers["Authorization"] = f"Bearer {admin_key}"

        response = requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/simulate-vram-pressure",
            json={"pct": pct},
            headers=headers,
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(
            response.status_code, 200, f"Simulate VRAM failed: {response.text}"
        )

    def test_eviction_vram_pressure(self):
        """VRAM pressure evicts the least-recently-used model."""
        admin_key = os.getenv("LEMONADE_ADMIN_API_KEY", "")
        headers = {}
        if admin_key:
            headers["Authorization"] = f"Bearer {admin_key}"

        requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json={
                "auto_evict": True,
                "auto_evict_threshold_pct": 0.90,
                "max_loaded_models": 2,
            },
            headers=headers,
        )

        # Load two models; sleep briefly so ENDPOINT_TEST_MODEL has an older access time
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(2)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": self.MODEL2},
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertIsNotNone(self._get_loaded_model_info(ENDPOINT_TEST_MODEL))
        self.assertIsNotNone(self._get_loaded_model_info(self.MODEL2))

        self._simulate_vram_pressure(0.95)

        # Poll until ENDPOINT_TEST_MODEL is evicted (or timeout)
        info1 = self._wait_for_model_status(
            ENDPOINT_TEST_MODEL, {None, "evicting", "unloaded"}
        )
        info2 = self._get_loaded_model_info(self.MODEL2)

        evicted = info1 is None or info1.get("status") in ("evicting", "unloaded")
        self.assertTrue(evicted, "Oldest model should be evicted")
        self.assertIsNotNone(info2, "Newer model should still be loaded")

    def test_downsize_idle_timeout(self):
        """Time-based idle timeout downsizes the model."""
        requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": ENDPOINT_TEST_MODEL,
                "auto_evict": True,
                "downsize_idle_timeout": 5,
                "evict_idle_timeout": 300,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertEqual(info.get("status"), "ready")

        # Poll until downsized (evaluation loop fires every 5s)
        info_after = self._wait_for_model_status(
            ENDPOINT_TEST_MODEL, {"downsized"}, timeout=15
        )
        self.assertIsNotNone(info_after)
        self.assertEqual(info_after.get("status"), "downsized")

    def test_request_interrupts_degradation_and_restores(self):
        """A request against a degraded (downsized) model must transparently
        restore it and succeed — no crash, no failed generation (spec #5)."""
        requests.post(
            f"{self.base_url}/load",
            json={
                "model_name": ENDPOINT_TEST_MODEL,
                "auto_evict": True,
                "downsize_idle_timeout": 3,
                "evict_idle_timeout": 300,
            },
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Let it slip into the downsized (degraded) tier
        info = self._wait_for_model_status(
            ENDPOINT_TEST_MODEL, {"downsized"}, timeout=15
        )
        self.assertIsNotNone(info)
        self.assertEqual(info.get("status"), "downsized")

        # Fire a real inference: this must interrupt the degradation and restore.
        client = self.get_openai_client()
        completion = client.chat.completions.create(
            model=ENDPOINT_TEST_MODEL,
            messages=self.messages,
            max_tokens=8,
        )
        self.assertTrue(completion.choices)
        self.assertIsNotNone(completion.choices[0].message.content)

        # After serving, the model should be resident again (ready/in_use), not downsized.
        info_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(
            info_after, "Model should still be loaded after the request"
        )
        self.assertIn(info_after.get("status"), ("ready", "in_use"))

    def test_weight_factor_protects_model(self):
        """A high evict_weight_factor should protect a model from VRAM-pressure
        eviction even when it is the older/idle one."""
        admin_key = os.getenv("LEMONADE_ADMIN_API_KEY", "")
        headers = {}
        if admin_key:
            headers["Authorization"] = f"Bearer {admin_key}"

        requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json={
                "auto_evict": True,
                "auto_evict_threshold_pct": 0.90,
                "max_loaded_models": 2,
            },
            headers=headers,
        )

        # Protected model loaded first (older) but with a large weight factor.
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": ENDPOINT_TEST_MODEL, "evict_weight_factor": 1000.0},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(2)
        requests.post(
            f"{self.base_url}/load",
            json={"model_name": self.MODEL2},
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        time.sleep(2)

        self._simulate_vram_pressure(0.95)

        # The newer, unweighted model should be the eviction target instead.
        info2 = self._wait_for_model_status(self.MODEL2, {None, "evicting", "unloaded"})
        evicted2 = info2 is None or info2.get("status") in ("evicting", "unloaded")
        self.assertTrue(evicted2, "Unweighted model should be evicted")
        self.assertIsNotNone(
            self._get_loaded_model_info(ENDPOINT_TEST_MODEL),
            "Heavily-weighted model should be protected",
        )

    def test_concurrent_unload_during_downsize_is_safe(self):
        """Stress the unload-vs-downsize lifetime race: while the eviction engine
        is downsizing an idle model, a concurrent unload must wait for the
        maintenance operation to finish rather than destroying the server out from
        under the engine. Regression test for the dangling-pointer race in the
        downsize path. The pass condition is simply that the server survives the
        churn (stays responsive, no crash/hang)."""
        admin_key = os.getenv("LEMONADE_ADMIN_API_KEY", "")
        headers = {}
        if admin_key:
            headers["Authorization"] = f"Bearer {admin_key}"

        # Very short downsize timeout so the engine is constantly trying to
        # downsize the model while we churn it underneath.
        requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json={"auto_evict": True},
            headers=headers,
        )

        stop = threading.Event()
        errors = []

        def load_model():
            requests.post(
                f"{self.base_url}/load",
                json={
                    "model_name": ENDPOINT_TEST_MODEL,
                    "auto_evict": True,
                    "downsize_idle_timeout": 1,
                    "evict_idle_timeout": 300,
                },
                timeout=TIMEOUT_MODEL_OPERATION,
            )

        def churn():
            try:
                while not stop.is_set():
                    load_model()
                    # Give the eviction engine a chance to begin a downsize.
                    time.sleep(1.5)
                    requests.post(
                        f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT
                    )
            except Exception as exc:  # noqa: BLE001 - surfaced via errors list
                errors.append(exc)

        threads = [threading.Thread(target=churn) for _ in range(3)]
        for t in threads:
            t.start()
        # Run the churn long enough to span several eviction-engine cycles (5s each).
        time.sleep(20)
        stop.set()
        for t in threads:
            t.join(timeout=TIMEOUT_MODEL_OPERATION)

        self.assertEqual(errors, [], f"Concurrent churn raised errors: {errors}")

        # The server must still be alive and responsive after the churn.
        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(health.status_code, 200, "Server should survive the churn")


if __name__ == "__main__":
    unittest.main()
