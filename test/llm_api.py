import unittest
import shutil
import os
from turnkeyml.state import State
import turnkeyml.common.filesystem as fs
import turnkeyml.common.test_helpers as common
from turnkeyml.llm.tools.huggingface_load import HuggingfaceLoad
from turnkeyml.llm.tools.mmlu import AccuracyMMLU
from turnkeyml.llm.tools.chat import LLMPrompt

ci_mode = os.getenv("LEMONADE_CI_MODE", False)


class Testing(unittest.TestCase):
    def setUp(self) -> None:
        shutil.rmtree(cache_dir, ignore_errors=True)

    def test_001_prompt(self):
        """
        Test the LLM Prompt tool
        """

        checkpoint = "facebook/opt-125m"
        prompt = "my solution to the test is"

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        state = LLMPrompt().run(state, prompt=prompt, max_new_tokens=15)

        assert len(state.response) > len(prompt), state.response
    
    def test_002_accuracy_mmlu(self):
        # Test MMLU benchmarking with known model
        checkpoint = "facebook/opt-125m"
        subject = ["management"]

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        state = AccuracyMMLU().run(state, ntrain=5, tests=subject)

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert stats[f"mmlu_{subject[0]}_accuracy"] > 0

    


if __name__ == "__main__":
    cache_dir, _ = common.create_test_dir("lemonade_api")
    unittest.main()
