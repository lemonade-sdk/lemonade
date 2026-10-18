"""Checks that differential validation reports regressions, not output noise."""

import unittest

from compare_baseline import diff, normalized
from harness import Response


def response(payload, status=200):
    import json

    return Response(
        status, {"content-type": "application/json"}, json.dumps(payload).encode()
    )


class DiffContractsTest(unittest.TestCase):
    def test_empty_diff(self):
        self.assertEqual(diff({"endpoint": 200}, {"endpoint": 200}), {})

    def test_missing_endpoint_is_reported(self):
        self.assertIn("endpoint", diff({"endpoint": 200}, {}))

    def test_health_ignores_version_and_volatile_fields(self):
        old = {
            "status": "ok",
            "version": "older",
            "model_loaded": None,
            "max_models": {"llm": 1},
        }
        new = {**old, "version": "new", "uptime": 200}
        self.assertEqual(
            normalized("/v1/health", response(old)),
            normalized("/v1/health", response(new)),
        )

    def test_model_inventory_order_is_ignored(self):
        one = {
            "data": [{"id": "a", "recipe": "llamacpp"}, {"id": "b", "recipe": "flm"}]
        }
        two = {"data": list(reversed(one["data"]))}
        self.assertEqual(
            normalized("/api/v1/models?show_all=true", response(one)),
            normalized("/api/v1/models?show_all=true", response(two)),
        )

    def test_model_inventory_differences_reported(self):
        a = normalized(
            "/api/v1/models?show_all=true", response({"data": [{"id": "a"}]})
        )
        b = normalized(
            "/api/v1/models?show_all=true", response({"data": [{"id": "b"}]})
        )
        self.assertNotEqual(a, b)

    def test_different_health_model_limits_are_reported(self):
        old = response({"status": "ok", "max_models": {"llm": 1}})
        new = response({"status": "ok", "max_models": {"llm": 2}})
        self.assertNotEqual(
            normalized("/v1/health", old), normalized("/v1/health", new)
        )

    def test_unexpected_http_error_is_reported(self):
        a = normalized("/v1/health", response({"status": "ok"}))
        b = normalized("/v1/health", response({"error": "crash"}, status=500))
        self.assertNotEqual(a, b)

    def test_http_error_status_kept(self):
        self.assertEqual(
            normalized("/any", response({"error": "missing"}, status=404))["status"],
            404,
        )


if __name__ == "__main__":
    unittest.main()
