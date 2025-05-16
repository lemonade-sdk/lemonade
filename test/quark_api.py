import unittest
import shutil
import os
from lemonade.state import State
import lemonade.common.test_helpers as common
from lemonade.tools.prompt import LLMPrompt
from lemonade.tools.huggingface_load import HuggingfaceLoad
from lemonade.tools.quark.quark_quantize import QuarkQuantize
from lemonade.tools.quark.quark_load import QuarkLoad


class Testing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Load default args from QuarkQuantize parser
        parser = QuarkQuantize.parser()
        cls.default_args = vars(parser.parse_args([]))

    def setUp(self) -> None:
        shutil.rmtree(cache_dir, ignore_errors=True)

    def test_001_quantize(self):
        """
        This test first quantizes the model, exports it to
        target format and then reloads the quantized model
        """
        checkpoint = "facebook/opt-125m"
        device = "cpu"
        prompt = "What if?"

        state = State(cache_dir=cache_dir, build_name="test")
        state = HuggingfaceLoad().run(state, input=checkpoint)

        quantize_args = {
            "model_export": "quark_safetensors",
            "quant_algo": "awq",
            "quant_scheme": "w_uint4_per_group_asym",
            "device": "cpu",
            "skip_quantization": True,
        }
        # Combine specific quant args with defaults
        quantize_args = {**self.default_args, **quantize_args}
        state = QuarkQuantize().run(state, **quantize_args)
        state = LLMPrompt().run(state, prompt=prompt, max_new_tokens=10)

        assert len(state.response) > 0, state.response


if __name__ == "__main__":
    cache_dir, _ = common.create_test_dir(
        "lemonade_quark_api", base_dir=os.path.abspath(".")
    )
    unittest.main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
