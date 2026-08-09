import os
import threading
import time

import requests
from utils.server_base import (
    ServerTestBase,
    pull_model_with_retry,
    run_server_tests,
)
from utils.test_models import (
    ENDPOINT_TEST_MODEL,
    MULTI_MODEL_TERTIARY,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)

IDLE_EVALUATION_PCT = -1.0
VRAM_PRESSURE_PCT = 0.95
VRAM_THRESHOLD_PCT = 0.90
TEST_CTX_SIZE = 256
MODEL_AGE_GAP_SECONDS = 0.01
RACE_STRESS_SECONDS = 4
RACE_CHURN_PAUSE_SECONDS = 0.01
RACE_EVALUATION_PAUSE_SECONDS = 0.005
RACE_REQUEST_TIMEOUT = 15


class EvictionTests(ServerTestBase):
    """Tests for dynamic VRAM eviction engine."""

    _model_pulled = False
    _runtime_config_before_suite = None
    _model2_pulled = False
    # Reuse the small LRU test model that server_endpoints.py already pulls in
    # the same hosted-Linux job instead of downloading/loading Phi-4-mini here.
    MODEL2 = MULTI_MODEL_TERTIARY

    @staticmethod
    def _admin_headers():
        admin_key = os.getenv("LEMONADE_ADMIN_API_KEY", "")
        return {"Authorization": f"Bearer {admin_key}"} if admin_key else {}

    @classmethod
    def setUpClass(cls):
        super().setUpClass()

        response = requests.get(
            f"http://localhost:{PORT}/internal/config",
            headers=cls._admin_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        response.raise_for_status()
        config = response.json()
        cls._runtime_config_before_suite = {
            key: config[key]
            for key in (
                "auto_evict",
                "auto_evict_threshold_pct",
                "max_loaded_models",
            )
            if key in config
        }
        cls.addClassCleanup(cls._restore_runtime_state)

        models_response = requests.get(
            f"http://localhost:{PORT}/api/v1/models",
            timeout=TIMEOUT_DEFAULT,
        )
        models_response.raise_for_status()
        downloaded_ids = {
            item.get("id") for item in models_response.json().get("data", [])
        }

        def is_downloaded(model_name):
            return (
                model_name in downloaded_ids
                or f"builtin.{model_name}" in downloaded_ids
            )

        if not cls._model_pulled:
            if is_downloaded(ENDPOINT_TEST_MODEL):
                print(f"\n[SETUP] Reusing downloaded {ENDPOINT_TEST_MODEL}")
            else:
                print(f"\n[SETUP] Ensuring {ENDPOINT_TEST_MODEL} is pulled...")
                pull_model_with_retry(ENDPOINT_TEST_MODEL)
            cls._model_pulled = True

        if not cls._model2_pulled:
            if is_downloaded(cls.MODEL2):
                print(f"\n[SETUP] Reusing downloaded {cls.MODEL2}")
            else:
                print(f"\n[SETUP] Ensuring {cls.MODEL2} is pulled...")
                pull_model_with_retry(cls.MODEL2)
            cls._model2_pulled = True

    @classmethod
    def _restore_runtime_state(cls):
        headers = cls._admin_headers()

        requests.post(
            f"http://localhost:{PORT}/api/v1/unload",
            json={},
            headers=headers,
            timeout=TIMEOUT_DEFAULT,
        )

        if cls._runtime_config_before_suite:
            response = requests.post(
                f"http://localhost:{PORT}/internal/set",
                json=cls._runtime_config_before_suite,
                headers=headers,
                timeout=TIMEOUT_DEFAULT,
            )
            response.raise_for_status()

    def setUp(self):
        super().setUp()
        response = requests.post(
            f"{self.base_url}/unload", json={}, timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(response.status_code, 200, response.text)

    def _set_eviction_config(self, **settings):
        response = requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/set",
            json=settings,
            headers=self._admin_headers(),
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200, response.text)

    def _load_model(self, model_name, **options):
        payload = {
            "model_name": model_name,
            # These tests only need residency/state transitions. Keeping the KV
            # cache tiny cuts llama.cpp startup and downsize work substantially.
            "ctx_size": TEST_CTX_SIZE,
        }
        payload.update(options)
        response = requests.post(
            f"{self.base_url}/load",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )
        self.assertEqual(
            response.status_code,
            200,
            f"Loading {model_name} failed: {response.text}",
        )
        return response

    def _get_loaded_model_info(self, model_name):
        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT).json()
        for model in health.get("all_models_loaded", []):
            if model["model_name"] == model_name:
                return model
        return None

    def _simulate_vram_pressure(self, pct, timeout=TIMEOUT_DEFAULT):
        response = requests.post(
            f"{self.base_url.replace('/api/v1', '')}/internal/simulate-vram-pressure",
            json={"pct": pct},
            headers=self._admin_headers(),
            timeout=timeout,
        )
        self.assertEqual(
            response.status_code, 200, f"Simulate VRAM failed: {response.text}"
        )

    def _evaluate_idle_now(self, timeout=TIMEOUT_DEFAULT):
        """Synchronously execute one idle-only eviction-engine evaluation."""
        self._simulate_vram_pressure(IDLE_EVALUATION_PCT, timeout=timeout)

    def test_eviction_vram_pressure(self):
        """VRAM pressure evicts the least-recently-used model."""
        self._set_eviction_config(
            auto_evict=True,
            auto_evict_threshold_pct=VRAM_THRESHOLD_PCT,
            max_loaded_models=2,
        )

        self._load_model(ENDPOINT_TEST_MODEL)
        # Loading MODEL2 itself makes the first model strictly older, so the old
        # 2-second age gap was pure wall-clock overhead.
        self._load_model(self.MODEL2)

        self.assertIsNotNone(self._get_loaded_model_info(ENDPOINT_TEST_MODEL))
        self.assertIsNotNone(self._get_loaded_model_info(self.MODEL2))

        self._simulate_vram_pressure(VRAM_PRESSURE_PCT)

        # The simulation endpoint invokes the eviction callback synchronously,
        # including the final evict_if_committed() call, so no polling is needed.
        info1 = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        info2 = self._get_loaded_model_info(self.MODEL2)

        evicted = info1 is None or info1.get("status") in ("evicting", "unloaded")
        self.assertTrue(evicted, "Oldest model should be evicted")
        self.assertIsNotNone(info2, "Newer model should still be loaded")

    def test_downsize_idle_timeout(self):
        """Time-based idle timeout downsizes the model."""
        self._set_eviction_config(
            auto_evict=True,
            auto_evict_threshold_pct=VRAM_THRESHOLD_PCT,
        )
        self._load_model(
            ENDPOINT_TEST_MODEL,
            auto_evict=True,
            downsize_idle_timeout=0,
            evict_idle_timeout=300,
        )

        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertEqual(info.get("status"), "ready")

        # Do not wait for the 5-second background cadence. The internal test hook
        # runs the same idle evaluation synchronously.
        self._evaluate_idle_now()

        info_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info_after)
        self.assertEqual(info_after.get("status"), "downsized")

    def test_request_interrupts_degradation_and_restores(self):
        """A request against a downsized model transparently restores it."""
        self._set_eviction_config(
            auto_evict=True,
            auto_evict_threshold_pct=VRAM_THRESHOLD_PCT,
        )
        self._load_model(
            ENDPOINT_TEST_MODEL,
            auto_evict=True,
            downsize_idle_timeout=0,
            evict_idle_timeout=300,
        )

        self._evaluate_idle_now()
        info = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(info)
        self.assertEqual(info.get("status"), "downsized")

        client = self.get_openai_client()
        completion = client.chat.completions.create(
            model=ENDPOINT_TEST_MODEL,
            messages=[{"role": "user", "content": "Hi"}],
            max_tokens=1,
        )
        self.assertTrue(completion.choices)
        self.assertIsNotNone(completion.choices[0].message.content)

        info_after = self._get_loaded_model_info(ENDPOINT_TEST_MODEL)
        self.assertIsNotNone(
            info_after, "Model should still be loaded after the request"
        )
        self.assertIn(info_after.get("status"), ("ready", "in_use"))

    def test_weight_factor_protects_model(self):
        """A high evict_weight_factor protects an older model under pressure."""
        self._set_eviction_config(
            auto_evict=True,
            auto_evict_threshold_pct=VRAM_THRESHOLD_PCT,
            max_loaded_models=2,
        )

        self._load_model(
            ENDPOINT_TEST_MODEL,
            evict_weight_factor=1_000_000_000_000.0,
        )
        self._load_model(self.MODEL2)
        # Give the unweighted model a non-zero idle score. The huge protection
        # factor makes this deterministic without the previous 4 seconds of sleeps.
        time.sleep(MODEL_AGE_GAP_SECONDS)

        self._simulate_vram_pressure(VRAM_PRESSURE_PCT)

        info2 = self._get_loaded_model_info(self.MODEL2)
        evicted2 = info2 is None or info2.get("status") in ("evicting", "unloaded")
        self.assertTrue(evicted2, "Unweighted model should be evicted")
        self.assertIsNotNone(
            self._get_loaded_model_info(ENDPOINT_TEST_MODEL),
            "Heavily-weighted model should be protected",
        )

    def test_concurrent_unload_during_downsize_is_safe(self):
        """Stress unload-vs-downsize without waiting on the 5-second scheduler."""
        self._set_eviction_config(
            auto_evict=True,
            auto_evict_threshold_pct=VRAM_THRESHOLD_PCT,
            max_loaded_models=2,
        )

        stop = threading.Event()
        errors = []

        def load_model():
            # Keep this helper assertion-free: transient HTTP status races are not
            # the pass condition here. Request exceptions and server death are.
            requests.post(
                f"{self.base_url}/load",
                json={
                    "model_name": ENDPOINT_TEST_MODEL,
                    "ctx_size": TEST_CTX_SIZE,
                    "auto_evict": True,
                    "downsize_idle_timeout": 0,
                    "evict_idle_timeout": 300,
                },
                timeout=RACE_REQUEST_TIMEOUT,
            )

        def churn():
            try:
                while not stop.is_set():
                    load_model()
                    # Leave a small overlap window for the forced idle pass to
                    # claim maintenance before unload takes the router lock.
                    time.sleep(RACE_CHURN_PAUSE_SECONDS)
                    requests.post(
                        f"{self.base_url}/unload",
                        json={},
                        timeout=RACE_REQUEST_TIMEOUT,
                    )
            except Exception as exc:  # noqa: BLE001 - surfaced via errors list
                errors.append(exc)
                stop.set()

        def drive_idle_evaluation():
            try:
                while not stop.is_set():
                    self._evaluate_idle_now(timeout=RACE_REQUEST_TIMEOUT)
                    time.sleep(RACE_EVALUATION_PAUSE_SECONDS)
            except Exception as exc:  # noqa: BLE001 - surfaced via errors list
                errors.append(exc)
                stop.set()

        # Prime one resident model before the timed stress window so the first
        # forced evaluation can immediately race an unload.
        self._load_model(
            ENDPOINT_TEST_MODEL,
            auto_evict=True,
            downsize_idle_timeout=0,
            evict_idle_timeout=300,
        )

        threads = [
            threading.Thread(target=churn),
            threading.Thread(target=drive_idle_evaluation),
        ]
        for thread in threads:
            thread.start()

        # Forced idle evaluations happen continuously, so four seconds provides
        # far more downsize opportunities than 20 seconds at a 5-second cadence.
        time.sleep(RACE_STRESS_SECONDS)
        stop.set()
        join_deadline = time.monotonic() + RACE_REQUEST_TIMEOUT + 1
        for thread in threads:
            thread.join(timeout=max(0.0, join_deadline - time.monotonic()))

        self.assertTrue(
            all(not thread.is_alive() for thread in threads),
            "Concurrent churn threads should terminate promptly",
        )
        self.assertEqual(errors, [], f"Concurrent churn raised errors: {errors}")

        health = requests.get(f"{self.base_url}/health", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(health.status_code, 200, "Server should survive the churn")


if __name__ == "__main__":
    run_server_tests(
        EvictionTests,
        description="VRAM EVICTION TESTS",
        modality="llm",
        default_wrapped_server="llamacpp",
    )
