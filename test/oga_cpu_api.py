import unittest
import shutil
import os
from lemonade.state import State
import lemonade.common.test_helpers as common
from lemonade.common.build import builds_dir
from lemonade.tools.prompt import LLMPrompt
from lemonade.tools.oga.load import OgaLoad
import sys

ci_mode = os.getenv("LEMONADE_CI_MODE", False)

checkpoint = "amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx"
device = "cpu"
dtype = "int4"
force = False
prompt = "Alice and Bob"


class Testing(unittest.TestCase):

    def setUp(self) -> None:
        shutil.rmtree(builds_dir(cache_dir), ignore_errors=True)

    def test_001_ogaload(self):
        # Test the OgaLoad and LLMPrompt tools on an NPU model

        state = State(cache_dir=cache_dir, build_name="test")

        state = OgaLoad().run(
            state, input=checkpoint, device=device, dtype=dtype, force=force
        )
        state = LLMPrompt().run(state, prompt=prompt, max_new_tokens=5)

        assert len(state.response) > 0, state.response


if __name__ == "__main__":
    cache_dir, _ = common.create_test_dir(
        "lemonade_oga_cpu_api", base_dir=os.path.abspath(".")
    )

    suite = unittest.TestSuite()
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(Testing))

    # Run the test suite
    runner = unittest.TextTestRunner()
    result = runner.run(suite)

    # Set exit code based on test results
    if not result.wasSuccessful():
        sys.exit(1)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
