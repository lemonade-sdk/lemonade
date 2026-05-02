"""
Lemonade Models (collection manifest) tests.

Verifies that POST /api/v1/pull accepts an embedded `lemonade_manifest` and
registers the collection plus any inline components without requiring an
HF round-trip. Tests use `register_only: true` so the recursive component
download is not triggered — registration alone is what we want to verify.

Local fixtures live in test/fixtures/lemonade_collection_*.json.

Requires a running server on PORT (defaults to 13305).

Usage:
    python server_collections.py
"""

import json
import os
import requests

from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    PORT,
    TIMEOUT_DEFAULT,
)

FIXTURES_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


def _load_fixture(name: str) -> dict:
    with open(os.path.join(FIXTURES_DIR, name), "r", encoding="utf-8") as f:
        return json.load(f)


class CollectionTests(ServerTestBase):
    """Tests for Lemonade Model (collection) registration via manifest."""

    # Names we register during the suite — cleaned up in tearDown.
    _registered_collections = (
        "user.test-refs-collection",
        "user.test-inline-collection",
    )
    _registered_inline_components = ("user.Test-Inline-LLM-GGUF",)

    def tearDown(self):
        """Best-effort cleanup of test-registered models."""
        super().tearDown()
        for name in self._registered_collections + self._registered_inline_components:
            try:
                requests.post(
                    f"{self.base_url}/delete",
                    json={"model_name": name},
                    timeout=TIMEOUT_DEFAULT,
                )
            except Exception:
                pass

    def _pull_manifest(self, model_name: str, manifest: dict, register_only: bool = True):
        body = {
            "model_name": model_name,
            "lemonade_manifest": manifest,
            "register_only": register_only,
        }
        return requests.post(
            f"{self.base_url}/pull",
            json=body,
            timeout=TIMEOUT_DEFAULT,
        )

    def _get_model(self, model_name: str):
        response = requests.get(
            f"{self.base_url}/models?show_all=true",
            timeout=TIMEOUT_DEFAULT,
        )
        self.assertEqual(response.status_code, 200)
        for m in response.json()["data"]:
            if m["id"] == model_name:
                return m
        return None

    def test_010_register_refs_collection(self):
        """A manifest with only string component refs registers cleanly."""
        manifest = _load_fixture("lemonade_collection_refs.json")
        response = self._pull_manifest("user.test-refs-collection", manifest)
        self.assertEqual(
            response.status_code, 200,
            f"pull failed: {response.status_code} {response.text}",
        )

        model = self._get_model("user.test-refs-collection")
        self.assertIsNotNone(model, "collection should appear in /models?show_all=true")
        self.assertEqual(model["recipe"], "collection")
        self.assertEqual(
            list(model["composite_models"]),
            [
                "Qwen3.5-35B-A3B-GGUF",
                "Flux-2-Klein-9B-GGUF",
                "Whisper-Large-v3-Turbo",
                "kokoro-v1",
            ],
        )

    def test_020_register_inline_collection(self):
        """An inline component is persisted as a user.-prefixed model."""
        manifest = _load_fixture("lemonade_collection_inline.json")
        response = self._pull_manifest("user.test-inline-collection", manifest)
        self.assertEqual(
            response.status_code, 200,
            f"pull failed: {response.status_code} {response.text}",
        )

        collection = self._get_model("user.test-inline-collection")
        self.assertIsNotNone(collection)
        self.assertEqual(collection["recipe"], "collection")
        self.assertEqual(
            list(collection["composite_models"]),
            ["user.Test-Inline-LLM-GGUF", "kokoro-v1"],
        )

        inline = self._get_model("user.Test-Inline-LLM-GGUF")
        self.assertIsNotNone(inline, "inline component should be registered")
        self.assertEqual(inline["recipe"], "llamacpp")
        self.assertIn("tool-calling", inline["labels"])

    def test_030_malformed_manifest_rejected(self):
        """A manifest with a non-whitelisted recipe is rejected."""
        manifest = _load_fixture("lemonade_collection_malformed.json")
        response = self._pull_manifest("user.test-bad-collection", manifest)
        self.assertNotEqual(
            response.status_code, 200,
            "manifest with disallowed recipe should be rejected",
        )

    def test_040_missing_user_prefix_rejected(self):
        """Manifest pulls without `user.` prefix are rejected."""
        manifest = _load_fixture("lemonade_collection_refs.json")
        response = self._pull_manifest("test-refs-no-prefix", manifest)
        self.assertNotEqual(
            response.status_code, 200,
            "collection pulls must require the `user.` prefix",
        )

    def test_050_unknown_string_component_rejected(self):
        """A manifest referencing a component not in the registry is rejected."""
        manifest = {
            "name": "Bad",
            "recipe": "collection",
            "components": ["this-component-does-not-exist"],
        }
        response = self._pull_manifest("user.test-bad-ref", manifest)
        self.assertNotEqual(
            response.status_code, 200,
            "unknown string components must be rejected",
        )


if __name__ == "__main__":
    run_server_tests(CollectionTests)
