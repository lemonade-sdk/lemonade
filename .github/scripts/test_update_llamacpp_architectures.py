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

# The parts of common.cpp and server-decision.cpp/.h that every shipped ref must have.
DECISION = """{ COMMON_DECISION_TYPE_LAYA, "laya" },
static const size_t DECISION_H2O_MAX_STATE_TOKENS = 32000;
    size_t      max_option_tokens = 48;
    if (n_options_tokens + 16 > max_head_tokens) {"""


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
        source = (
            '{ LLM_ARCH_GEMMA4, "gemma4" },\n{ LLM_ARCH_QWEN35MOE, "qwen35moe" },\n'
            + DECISION
        )

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

    def test_decision_types_are_the_common_denominator(self):
        arch = '{ LLM_ARCH_QWEN35, "qwen35" },\n' + DECISION
        d3 = '\n{ COMMON_DECISION_TYPE_VLLM_D3, "vllm-d3" },'

        def upstream(ref):
            return arch if ref == "b11819" else arch + d3

        header = self.run_generator(upstream, lambda ref: arch + d3)
        decision = header.partition("llamacpp_decision_type_is_supported")[2]
        self.assertIn('"laya"', decision)
        self.assertNotIn('"vllm-d3"', decision)

    def test_a_ref_without_laya_fails(self):
        arch = '{ LLM_ARCH_QWEN35, "qwen35" },\n'
        without_laya = arch + DECISION.replace("LAYA", "LEV").replace('"laya"', '"lev"')

        def upstream(ref):
            return without_laya if ref == "b11819" else arch + DECISION

        with self.assertRaisesRegex(SystemExit, "'laya' is missing"):
            self.run_generator(upstream, lambda ref: arch + DECISION)

    def test_decision_limits_come_from_server_decision(self):
        arch = '{ LLM_ARCH_QWEN35, "qwen35" },\n'
        header = self.run_generator(
            lambda ref: arch + DECISION, lambda ref: arch + DECISION
        )
        self.assertIn("LLAMACPP_DECISION_H2O_MAX_STATE_TOKENS = 32000;", header)
        self.assertIn("LLAMACPP_DECISION_LAYA_MAX_OPTION_TOKENS = 48;", header)
        self.assertIn("LLAMACPP_DECISION_LAYA_QUESTION_RESERVE = 16;", header)

        changed = arch + DECISION.replace("= 48", "= 64")
        with self.assertRaisesRegex(SystemExit, "the shipped refs disagree"):
            self.run_generator(
                lambda ref: changed if ref == "b11819" else arch + DECISION,
                lambda ref: arch + DECISION,
            )

        renamed = arch + DECISION.replace("max_option_tokens", "option_budget")
        with self.assertRaisesRegex(
            SystemExit, "LAYA_MAX_OPTION_TOKENS: no single value"
        ):
            self.run_generator(lambda ref: renamed, lambda ref: renamed)

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
