import unittest
import shutil
import os
import urllib3
from turnkeyml.state import State
import turnkeyml.common.filesystem as fs
import turnkeyml.common.test_helpers as common
from turnkeyml.llm.tools.huggingface_load import HuggingfaceLoad
from turnkeyml.llm.tools.huggingface_bench import HuggingfaceBench
from turnkeyml.llm.tools.mmlu import AccuracyMMLU
from turnkeyml.llm.tools.chat import LLMPrompt
from turnkeyml.llm.cache import Keys

ci_mode = os.getenv("LEMONADE_CI_MODE", False)

try:
    url = "https://people.eecs.berkeley.edu/~hendrycks/data.tar"
    resp = urllib3.request("GET", url, preload_content=False)
    if 200 <= resp.status < 400:
        eecs_berkeley_edu_cannot_be_reached = False
    else:
        eecs_berkeley_edu_cannot_be_reached = True
    resp.release_conn()
except urllib3.exceptions.HTTPError:
    eecs_berkeley_edu_cannot_be_reached = True


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

    @unittest.skipIf(eecs_berkeley_edu_cannot_be_reached, "eecs.berkeley.edu cannot be reached for dataset download")
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

    def test_001_huggingface_bench(self):
        # Benchmark OPT
        checkpoint = "facebook/opt-125m"

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        state = HuggingfaceBench().run(state, iterations=20)

        stats = fs.Stats(state.cache_dir, state.build_name).stats

        assert stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND] > 0


if __name__ == "__main__":
    cache_dir, _ = common.create_test_dir("lemonade_api")
    unittest.main()
