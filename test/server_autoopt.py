"""AutoOpt wizard endpoint tests.

Quick-tier runs need no GPU benchmarks (fit probes + heuristics only), so the
whole suite works on CI-class machines as long as the test model is pulled.
"""

import sys
import time
import unittest

import requests

sys.path.insert(0, ".")
sys.path.insert(0, "test")

from utils.server_base import ServerTestBase, TIMEOUT_DEFAULT
from utils.test_models import ENDPOINT_TEST_MODEL


def _wait_for_run(base_url, run_id, timeout_s=120):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        resp = requests.get(
            f"{base_url}/autoopt/runs/{run_id}", timeout=TIMEOUT_DEFAULT
        )
        if resp.status_code == 200:
            last = resp.json()
            if last.get("status") in ("completed", "failed", "cancelled"):
                return last
        time.sleep(1)
    return last


class AutoOptEndpointTests(ServerTestBase):
    def _pull_test_model(self):
        resp = requests.post(
            f"{self.base_url}/pull",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=600,
        )
        self.assertIn(resp.status_code, (200, 201))

    def _start(self, body):
        return requests.post(
            f"{self.base_url}/autoopt/start", json=body, timeout=TIMEOUT_DEFAULT
        )

    def test_001_list_runs_empty_ok(self):
        resp = requests.get(f"{self.base_url}/autoopt/runs", timeout=TIMEOUT_DEFAULT)
        self.assertEqual(resp.status_code, 200)
        self.assertIn("runs", resp.json())

    def test_002_start_validation(self):
        resp = self._start({})
        self.assertEqual(resp.status_code, 400)
        resp = self._start({"model": ENDPOINT_TEST_MODEL, "budget": "bogus"})
        self.assertEqual(resp.status_code, 400)

    def test_003_quick_run_e2e(self):
        self._pull_test_model()
        resp = self._start(
            {
                "model": ENDPOINT_TEST_MODEL,
                "budget": "quick",
                "allow_unload": False,
                "answers": {
                    "parallel": {"mode": "single"},
                    "kv_cache_quant": "q8_0",
                    "ram_headroom": "reduced",
                    "allow_network": False,
                },
            }
        )
        self.assertEqual(resp.status_code, 202, resp.text)
        run_id = resp.json()["id"]

        run = _wait_for_run(self.base_url, run_id)
        self.assertIsNotNone(run, "run never became visible")
        self.assertEqual(run.get("status"), "completed", run)

        stage_names = [s["name"] for s in run["stages"]]
        self.assertIn("snapshot", stage_names)
        self.assertIn("synthesize", stage_names)
        by_name = {s["name"]: s for s in run["stages"]}
        self.assertEqual(by_name["hf_metadata"]["status"], "skipped")
        self.assertEqual(by_name["bench_matrix"]["status"], "skipped")

        result = run.get("result")
        self.assertIsInstance(result, dict, run)
        primary = result["primary"]
        self.assertIn("-ctk q8_0", primary["llamacpp_args"])
        self.assertIn("--cache-ram 4096", primary["llamacpp_args"])
        self.assertIn("--spec-default", primary["llamacpp_args"])
        self.assertGreater(primary["ctx_size"], 0)
        self.assertTrue(primary["rationale"], "rationale must not be empty")
        self.assertTrue(result["alternatives"], "expected alternatives")

        listing = requests.get(
            f"{self.base_url}/autoopt/runs", timeout=TIMEOUT_DEFAULT
        ).json()["runs"]
        self.assertTrue(any(r["id"] == run_id for r in listing))
        summary = next(r for r in listing if r["id"] == run_id)
        self.assertEqual(summary["model"], ENDPOINT_TEST_MODEL)
        self.assertIn("created_at", summary)

    def test_004_apply_persists_options(self):
        listing = requests.get(
            f"{self.base_url}/autoopt/runs", timeout=TIMEOUT_DEFAULT
        ).json()["runs"]
        completed = [r for r in listing if r["status"] == "completed"]
        if not completed:
            self.skipTest("no completed run to apply")
        run_id = completed[0]["id"]
        resp = requests.post(
            f"{self.base_url}/autoopt/apply",
            json={"id": run_id, "preset_index": 0},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 200, resp.text)
        saved = resp.json()["options"]
        self.assertIn("llamacpp_args", saved)

        detail = requests.get(
            f"{self.base_url}/models/{ENDPOINT_TEST_MODEL}", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(detail.status_code, 200)
        persisted = detail.json().get("recipe_options", {})
        self.assertEqual(persisted.get("llamacpp_args"), saved["llamacpp_args"])
        self.assertEqual(persisted.get("ctx_size"), saved["ctx_size"])

    def test_005_cancel_and_delete(self):
        resp = self._start(
            {
                "model": ENDPOINT_TEST_MODEL,
                "budget": "quick",
                "answers": {"allow_network": False},
            }
        )
        self.assertEqual(resp.status_code, 202, resp.text)
        run_id = resp.json()["id"]

        cancel = requests.post(
            f"{self.base_url}/autoopt/cancel",
            json={"id": run_id},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(cancel.status_code, 200)

        run = _wait_for_run(self.base_url, run_id)
        self.assertIn(run.get("status"), ("cancelled", "completed"))

        delete = requests.delete(
            f"{self.base_url}/autoopt/runs/{run_id}", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(delete.status_code, 200)
        gone = requests.get(
            f"{self.base_url}/autoopt/runs/{run_id}", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(gone.status_code, 404)

    def test_006_unknown_run_404(self):
        resp = requests.get(
            f"{self.base_url}/autoopt/runs/ao-none", timeout=TIMEOUT_DEFAULT
        )
        self.assertEqual(resp.status_code, 404)
        resp = requests.post(
            f"{self.base_url}/autoopt/cancel",
            json={"id": "ao-none"},
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(resp.status_code, 404)


if __name__ == "__main__":
    unittest.main(verbosity=2)
