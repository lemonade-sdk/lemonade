"""llama.cpp tool endpoint tests (backends/llamacpp/fit-params and /bench).

fit-params is a quick estimator (no inference), so the suite works on
CI-class machines as long as the test model is pulled and a llamacpp
backend is installed. The bench test streams a real llama-bench run on the
tiny endpoint-test model.
"""

import json
import sys
import unittest

import requests

sys.path.insert(0, ".")
sys.path.insert(0, "test")

from utils.server_base import ServerTestBase, TIMEOUT_DEFAULT
from utils.test_models import ENDPOINT_TEST_MODEL


def _sse_events(resp):
    event, data = None, []
    for raw in resp.iter_lines(decode_unicode=True):
        if raw is None:
            continue
        if raw.startswith("event: "):
            event = raw[len("event: ") :]
        elif raw.startswith("data: "):
            data.append(raw[len("data: ") :])
        elif raw == "" and event is not None:
            yield event, json.loads("\n".join(data)) if data else None
            event, data = None, []


class LlamaCppToolsEndpointTests(ServerTestBase):
    def _pull_test_model(self):
        resp = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=600,
        )
        self.assertIn(resp.status_code, (200, 201))

    def _installed_backend(self):
        resp = requests.get(f"{self.base_url}/system-info", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(resp.status_code, 200)
        backends = (
            resp.json().get("recipes", {}).get("llamacpp", {}).get("backends", {})
        )
        for name, state in backends.items():
            if state.get("state") in ("installed", "update_available"):
                return name
        return None

    def test_001_fit_params_validation(self):
        resp = requests.post(
            f"{self.base_url}/backends/llamacpp/fit-params",
            json={},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 400)

        resp = requests.post(
            f"{self.base_url}/backends/llamacpp/fit-params",
            json={"model": "no-such-model", "backend": "vulkan"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 404)

    def test_002_fit_params_get_is_405(self):
        resp = requests.get(
            f"{self.base_url}/backends/llamacpp/fit-params", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(resp.status_code, 405)

    def test_003_fit_params_e2e(self):
        backend = self._installed_backend()
        if backend is None:
            self.skipTest("no llamacpp backend installed")
        self._pull_test_model()

        resp = requests.post(
            f"{self.base_url}/backends/llamacpp/fit-params",
            json={"model": ENDPOINT_TEST_MODEL, "backend": backend},
            timeout=180,
        )
        self.assertEqual(resp.status_code, 200, resp.text)
        fit = resp.json()
        self.assertTrue(fit.get("ok"), fit)
        self.assertEqual(fit.get("backend"), backend)
        self.assertIsInstance(fit.get("devices"), list)

        resp = requests.post(
            f"{self.base_url}/backends/llamacpp/fit-params",
            json={
                "model": ENDPOINT_TEST_MODEL,
                "backend": backend,
                "args": "-c 4096 -ctk q8_0 -ctv q8_0",
            },
            timeout=180,
        )
        self.assertEqual(resp.status_code, 200, resp.text)
        self.assertEqual(resp.json().get("extra_args"), "-c 4096 -ctk q8_0 -ctv q8_0")

    def test_004_bench_e2e_streaming(self):
        backend = self._installed_backend()
        if backend is None:
            self.skipTest("no llamacpp backend installed")
        self._pull_test_model()

        resp = requests.post(
            f"{self.base_url}/backends/llamacpp/bench",
            json={"model": ENDPOINT_TEST_MODEL, "backend": backend, "d": 0},
            stream=True,
            timeout=600,
        )
        self.assertEqual(resp.status_code, 200)
        complete = None
        for event, data in _sse_events(resp):
            if event == "complete":
                complete = data
                break
        self.assertIsNotNone(complete, "no complete event")
        points = complete.get("points", [])
        self.assertTrue(points, complete)
        self.assertTrue(points[0].get("ok"), points)
        self.assertGreater(points[0].get("tg_avg_ts", 0), 0, points)

    def test_005_models_metadata_block(self):
        self._pull_test_model()
        resp = requests.get(
            f"{self.base_url}/models/{ENDPOINT_TEST_MODEL}", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(resp.status_code, 200)
        meta = resp.json().get("metadata")
        self.assertIsInstance(meta, dict, resp.json())
        self.assertTrue(meta.get("architecture"))
        self.assertGreater(meta.get("context_length", 0), 0)
        self.assertIn("expert_count", meta)
        self.assertIn("kv_bytes_per_token", meta)


if __name__ == "__main__":
    unittest.main(verbosity=2)
