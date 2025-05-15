import unittest
import shutil
import os
import urllib3
from turnkeyml.state import State
import turnkeyml.common.test_helpers as common
import turnkeyml.common.filesystem as fs
from turnkeyml.common.build import builds_dir
from lemonade.cache import Keys
from lemonade.tools.ort_genai.oga import OgaLoad, OrtGenaiModel
from lemonade.tools.prompt import LLMPrompt
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.humaneval import AccuracyHumaneval
from lemonade.tools.ort_genai.oga_bench import OgaBench
import sys

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


class TestOgaLogprobs(unittest.TestCase):
    """Test the compute_logprobs functionality in OGA implementation using direct OgaLoad adapter"""

    def setUp(self) -> None:
        # Use a unique build name for each test to avoid conflicts
        self.build_name = f"test_oga_logprobs"

    def test_001_compute_logprobs_completion(self):
        """Test compute_logprobs with a specific completion for OGA models"""

        state = State(
            cache_dir=cache_dir,
            build_name=self.build_name,
        )

        # Load model directly using OgaLoad
        state = OgaLoad().run(state, input=checkpoint, device="cpu", dtype="int4")

        # Ensure model is an OrtGenaiModel instance
        self.assertIsInstance(state.model, OrtGenaiModel)

        # Ensure model has compute_logprobs method
        self.assertTrue(
            hasattr(state.model, "compute_logprobs"),
            "OGA model should have compute_logprobs method",
        )

        # Test with simple prompt
        text = "The capital of France is Paris"

        result = state.model.compute_logprobs(
            text=text, tokenizer=state.tokenizer, logprobs=5
        )

        # Verify output structure - OGA returns a tuple, not a dictionary
        self.assertIsInstance(result, tuple)
        self.assertEqual(len(result), 4)  # Should have 4 elements

        text_offset, token_logprobs, tokens, top_logprobs = result

        # Check the content of the output
        self.assertEqual(len(tokens), len(token_logprobs))
        self.assertEqual(len(tokens), len(text_offset))
        self.assertEqual(len(tokens), len(top_logprobs))

    def test_002_compute_logprobs_echo_parameter(self):
        """Test compute_logprobs with echo parameter controlling prompt token inclusion for OGA models"""

        state = State(
            cache_dir=cache_dir,
            build_name=self.build_name,
        )

        # Load model directly using OgaLoad
        state = OgaLoad().run(state, input=checkpoint, device="cpu", dtype="int4")

        # Define test inputs with clear prompt and completion portions
        prefix = "This is the prompt section."
        completion = "This is the completion section."
        full_text = prefix + " " + completion

        # First encode the prefix to get prompt length
        prompt_tokens = state.tokenizer(prefix).input_ids
        prompt_length = len(prompt_tokens)

        # Test with echo=False
        text_offset_no_echo, token_logprobs_no_echo, tokens_no_echo, _ = (
            state.model.compute_logprobs(
                text=full_text,
                tokenizer=state.tokenizer,
                prompt_length=prompt_length,
                logprobs=5,
                echo=False,
            )
        )

        # Test with echo=True
        text_offset_with_echo, token_logprobs_with_echo, tokens_with_echo, _ = (
            state.model.compute_logprobs(
                text=full_text,
                tokenizer=state.tokenizer,
                prompt_length=prompt_length,
                logprobs=5,
                echo=True,
            )
        )

        # Verify that echo=False returns only completion tokens
        self.assertLess(len(tokens_no_echo), len(tokens_with_echo))

        # Verify echo=True includes all tokens
        self.assertGreaterEqual(len(tokens_with_echo), prompt_length)

        # Test that all returned tokens in echo=False have logprobs
        for lp in token_logprobs_no_echo:
            if lp is not None:  # Skip None values which happen for first token
                self.assertIsInstance(lp, float)

        # Test the extreme case where prompt_length=0 (everything is treated as completion)
        zero_prompt_text = "All of this is completion."
        zero_offset, zero_logprobs, zero_tokens, _ = state.model.compute_logprobs(
            text=zero_prompt_text,
            tokenizer=state.tokenizer,
            prompt_length=0,
            logprobs=5,
            echo=False,
        )

        # All tokens should be included when prompt_length=0
        all_tokens = state.tokenizer(zero_prompt_text).input_ids
        self.assertEqual(len(zero_tokens), len(all_tokens))


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

    # Create a test suite with all test classes
    suite = unittest.TestSuite()
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(Testing))
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestOgaLogprobs))

    # Run the test suite
    runner = unittest.TextTestRunner()
    result = runner.run(suite)

    # Set exit code based on test results
    if not result.wasSuccessful():
        sys.exit(1)
