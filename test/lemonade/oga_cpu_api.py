import unittest
import shutil
import os
import urllib3
from turnkeyml.state import State
import turnkeyml.common.test_helpers as common
import turnkeyml.common.filesystem as fs
from turnkeyml.common.build import builds_dir
from lemonade.cache import Keys
from lemonade.tools.ort_genai.oga import OgaLoad
from lemonade.tools.prompt import LLMPrompt
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.humaneval import AccuracyHumaneval
from lemonade.tools.ort_genai.oga_bench import OgaBench

ci_mode = os.getenv("LEMONADE_CI_MODE", False)

checkpoint = "TinyPixel/small-llama2"
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

    def test_002_accuracy_mmlu(self):
        # Test MMLU benchmarking with known model
        subject = ["management"]

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = OgaLoad().run(state, input=checkpoint, device=device, dtype=dtype)
        state = AccuracyMMLU().run(state, ntrain=5, tests=subject)

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert stats[f"mmlu_{subject[0]}_accuracy"] >= 0

    def test_003_accuracy_humaneval(self):
        """Test HumanEval benchmarking with known model"""

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        # Enable code evaluation for HumanEval
        os.environ["HF_ALLOW_CODE_EVAL"] = "1"

        state = OgaLoad().run(state, input=checkpoint, device=device, dtype=dtype)
        state = AccuracyHumaneval().run(
            state,
            first_n_samples=1,  # Test only one problem for speed
            k_samples=1,  # Single attempt per problem
            timeout=30.0,
        )

        # Verify results
        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert "humaneval_pass@1" in stats, "HumanEval pass@1 metric not found"
        assert isinstance(
            stats["humaneval_pass@1"], (int, float)
        ), "HumanEval pass@1 metric should be numeric"

    def test_004_oga_multiple_bench(self):
        """Test OgaBench with multiple prompts"""

        state = State(cache_dir=cache_dir, build_name="test")

        state = OgaLoad().run(state, input=checkpoint, device=device, dtype=dtype)
        state = OgaBench().run(
            state, iterations=20, prompts=["word " * 30, "word " * 62]
        )

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert len(stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND]) == 2
        assert all(x > 0 for x in stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND])


if __name__ == "__main__":
    cache_dir, _ = common.create_test_dir(
        "lemonade_oga_cpu_api", base_dir=os.path.abspath(".")
    )

    # Get MMLU data
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

    unittest.main()
