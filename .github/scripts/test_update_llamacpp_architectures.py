"""Offline regression tests for llama.cpp architecture source selection."""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("update_llamacpp_architectures.py")
spec = importlib.util.spec_from_file_location("update_llamacpp_architectures", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ArchitectureSourceTests(unittest.TestCase):
    def run_generator(self, upstream, rocm):
        with tempfile.TemporaryDirectory() as directory:
            versions = Path(directory) / "versions.json"
            output = Path(directory) / "architectures.h"
            versions.write_text(
                json.dumps(
                    {
                        "llamacpp": {
                            "cpu": "b11823",
                            "cuda": "b11819",
                            "rocm-nightly": "b1352",
                        }
                    }
                ),
                encoding="utf-8",
            )
            with patch.object(
                sys,
                "argv",
                ["generator", "--versions", str(versions), "--output", str(output)],
            ), patch.object(
                module, "fetch_upstream_source", side_effect=upstream
            ), patch.object(
                module, "fetch_rocm_nightly_source", side_effect=rocm
            ):
                module.main()
            return output.read_text(encoding="utf-8")

    def test_colliding_rocm_tag_never_fetches_upstream_tag(self):
        upstream_calls = []
        rocm_calls = []
        source = '{ LLM_ARCH_GEMMA4, "gemma4" },\n{ LLM_ARCH_QWEN35MOE, "qwen35moe" },'

        def upstream(ref):
            upstream_calls.append(ref)
            if ref == "b1352":
                return '{ LLM_ARCH_LLAMA, "llama" },'  # Old *upstream* tag collision
            return source

        def rocm(ref):
            rocm_calls.append(ref)
            return source

        header = self.run_generator(upstream, rocm)
        self.assertEqual(upstream_calls, ["b11819", "b11823"])
        self.assertEqual(rocm_calls, ["b1352"])
        self.assertIn('"gemma4"', header)
        self.assertIn('"qwen35moe"', header)
        self.assertNotIn('"llama",', header)

    def test_missing_rocm_release_fails_closed(self):
        upstream_calls = []

        def upstream(ref):
            upstream_calls.append(ref)
            return '{ LLM_ARCH_GEMMA4, "gemma4" },'

        def rocm(ref):
            raise ValueError("Unresolvable ROCm commit")

        with self.assertRaisesRegex(
            RuntimeError, "No architecture source found for shipped ref b1352"
        ):
            self.run_generator(upstream, rocm)
        self.assertEqual(upstream_calls, ["b11819", "b11823"])


if __name__ == "__main__":
    unittest.main()
