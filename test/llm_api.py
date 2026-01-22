"""
Tests for Lemonade SDK using server_load and server_bench tools.

These tests require a running Lemonade Server instance.
Start the server before running tests with: lemonade-server serve

Uses Qwen3-4B-Instruct-2507-GGUF as the test model (automatically downloaded by lemonade-server).
"""

import unittest
import shutil
import os
import sys
import logging

import requests
import urllib3

from lemonade.state import State
import lemonade.common.filesystem as fs
import lemonade.common.test_helpers as common
import lemonade.common.build as build
from lemonade.tools.server_load import Load as ServerLoad
from lemonade.tools.server_bench import ServerBench
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.humaneval import AccuracyHumaneval
from lemonade.tools.accuracy import LMEvalHarness
from lemonade.tools.prompt import LLMPrompt
from lemonade.cache import Keys

# Configure logging
logging.basicConfig(
    level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Define cache_dir and corpus_dir at the module level
cache_dir = None
corpus_dir = None

# Test model - Lemonade Server will download this automatically
TEST_MODEL = "Qwen3-4B-Instruct-2507-GGUF"
SERVER_URL = os.getenv("LEMONADE_SERVER_URL", "http://localhost:8000")


class TestServerIntegration(unittest.TestCase):
    """
    Integration tests using Lemonade Server with server_load and server_bench.

    Requires a running Lemonade Server instance.
    """

    @classmethod
    def setUpClass(cls):
        """Check that Lemonade Server is running"""
        logger.info(f"Testing against Lemonade Server at {SERVER_URL}")
        logger.info(f"Using test model: {TEST_MODEL}")

        try:
            response = requests.get(f"{SERVER_URL}/api/v1/health", timeout=10)
            response.raise_for_status()
            logger.info("Lemonade Server is running and healthy")
        except requests.exceptions.ConnectionError as e:
            raise ConnectionError(
                f"Cannot connect to Lemonade Server at {SERVER_URL}. "
                "Start the server with 'lemonade-server serve' before running tests."
            ) from e
        except Exception as e:
            raise RuntimeError(f"Error connecting to Lemonade Server: {e}") from e

    def setUp(self):
        shutil.rmtree(cache_dir, ignore_errors=True)
        self.state = State(
            cache_dir=cache_dir,
            build_name="test_server",
        )

    def test_001_load_model(self):
        """Test loading a model via Lemonade Server"""
        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )

        self.assertIsNotNone(state.model)
        self.assertIsNotNone(state.tokenizer)
        self.assertEqual(state.checkpoint, TEST_MODEL)

    def test_002_prompt(self):
        """Test the LLM Prompt tool via Lemonade Server"""
        prompt = "What is the capital of France?"

        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )
        state = LLMPrompt().run(state, prompt=prompt, max_new_tokens=20)

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        self.assertIn("response", stats)
        self.assertGreater(len(stats["response"]), 0, stats["response"])

    def test_003_benchmark(self):
        """Test benchmarking via Lemonade Server"""
        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )

        state = ServerBench().run(
            state,
            iterations=2,
            warmup_iterations=1,
            output_tokens=128,
            prompts=["Tell me an extremely long story about pirates."],
        )

        # Check if we got valid metrics
        stats = fs.Stats(state.cache_dir, state.build_name).stats
        self.assertIn(Keys.TOKEN_GENERATION_TOKENS_PER_SECOND, stats)
        self.assertIn(Keys.SECONDS_TO_FIRST_TOKEN, stats)

    def test_004_benchmark_multiple_prompts(self):
        """Test benchmarking with multiple prompts"""
        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )

        state = ServerBench().run(
            state,
            iterations=5,
            prompts=["word " * 30, "word " * 62],
        )

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        self.assertEqual(len(stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND]), 2)
        self.assertTrue(
            all(x > 0 for x in stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND])
        )

    def test_005_prompt_from_file(self):
        """Test the LLM Prompt tool capability to load prompt from a file"""
        prompt_str = "Who is Humpty Dumpty?"

        prompt_path = os.path.join(corpus_dir, "prompt.txt")
        with open(prompt_path, "w", encoding="utf-8") as f:
            f.write(prompt_str)

        llm_prompt_args = ["-p", prompt_path, "--max-new-tokens", "15"]

        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )
        llm_prompt_kwargs = LLMPrompt().parse(state, llm_prompt_args).__dict__
        state = LLMPrompt().run(state, **llm_prompt_kwargs)

        stats = fs.Stats(state.cache_dir, state.build_name).stats

        self.assertGreater(len(stats["response"]), 0, stats["response"])
        self.assertEqual(stats["prompt"], prompt_str, f"{stats['prompt']} {prompt_str}")

    def test_006_multiple_prompt_responses(self):
        """Test the LLM Prompt tool capability to run multiple inferences on the same prompt"""
        prompt_str = "Who is Humpty Dumpty?"
        n_trials = 2

        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )
        state = LLMPrompt().run(
            state, prompt=prompt_str, max_new_tokens=15, n_trials=n_trials
        )

        stats = fs.Stats(state.cache_dir, state.build_name).stats

        # Check that two responses were generated
        self.assertIsInstance(stats["response"], list)
        self.assertEqual(len(stats["response"]), n_trials)
        self.assertIsInstance(stats["response_tokens"], list)
        self.assertEqual(len(stats["response_tokens"]), n_trials)

        # Check that histogram figure was generated
        self.assertTrue(
            os.path.exists(
                os.path.join(
                    build.output_dir(state.cache_dir, state.build_name),
                    "response_lengths.png",
                )
            )
        )

    def test_007_accuracy_mmlu(self):
        """Test MMLU benchmarking via Lemonade Server"""
        subject = ["management"]

        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )
        # Use max_evals=1 to keep test fast
        state = AccuracyMMLU().run(state, ntrain=5, tests=subject, max_evals=1)

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        self.assertGreaterEqual(stats[f"mmlu_{subject[0]}_accuracy"], 0)

    def test_008_accuracy_humaneval(self):
        """Test HumanEval benchmarking via Lemonade Server"""
        # Enable code evaluation for HumanEval
        os.environ["HF_ALLOW_CODE_EVAL"] = "1"

        state = ServerLoad().run(
            self.state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )
        state = AccuracyHumaneval().run(
            state,
            first_n_samples=1,  # Test only one problem for speed
            k_samples=1,  # Single attempt per problem
            timeout=30.0,
        )

        # Verify results
        stats = fs.Stats(state.cache_dir, state.build_name).stats
        self.assertIn("humaneval_pass@1", stats, "HumanEval pass@1 metric not found")
        self.assertIsInstance(
            stats["humaneval_pass@1"],
            (int, float),
            "HumanEval pass@1 metric should be numeric",
        )

    def test_009_accuracy_lmeval(self):
        """Test lm-eval-harness benchmarking via Lemonade Server"""
        state = State(
            cache_dir=cache_dir,
            build_name="test_lmeval",
        )

        state = ServerLoad().run(
            state,
            input=TEST_MODEL,
            server_url=SERVER_URL,
        )
        # Use gsm8k which uses generate_until (doesn't require logprobs)
        state = LMEvalHarness().run(
            state,
            task="gsm8k",
            limit=1,
            num_fewshot=0,
        )

        # Verify results
        stats = fs.Stats(state.cache_dir, state.build_name).stats

        # Check if any lm_eval stats were saved
        lm_eval_stats = [k for k in stats.keys() if k.startswith("lm_eval_")]
        self.assertGreater(
            len(lm_eval_stats), 0, "No lm-eval-harness metrics found in stats"
        )

        results_dir = os.path.join(
            build.output_dir(state.cache_dir, state.build_name), "lm_eval_results"
        )
        self.assertTrue(
            os.path.exists(results_dir), f"Results directory not found: {results_dir}"
        )
        json_files = [f for f in os.listdir(results_dir) if f.endswith(".json")]
        self.assertGreater(
            len(json_files), 0, f"No JSON results file found in {results_dir}"
        )

        # Check for gsm8k exact_match metric
        self.assertIn(
            "lm_eval_gsm8k_exact_match",
            stats,
            "Expected lm_eval_gsm8k_exact_match metric not found",
        )
        self.assertIsInstance(
            stats["lm_eval_gsm8k_exact_match"],
            (int, float),
            "lm_eval_gsm8k_exact_match should be numeric",
        )


if __name__ == "__main__":
    # Get cache directory from environment or create a new one
    cache_dir = os.getenv("LEMONADE_CACHE_DIR")
    if not cache_dir:
        # Create test directories
        cache_dir, corpus_dir = common.create_test_dir("lemonade_api")
        os.environ["LEMONADE_CACHE_DIR"] = cache_dir
    else:
        corpus_dir = os.path.join(os.path.dirname(cache_dir), "corpus")
        os.makedirs(corpus_dir, exist_ok=True)

    logger.info(f"Using cache directory: {cache_dir}")
    logger.info(f"Using corpus directory: {corpus_dir}")

    # Download mmlu
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

    # Create test suite with all test classes
    suite = unittest.TestSuite()
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestServerIntegration))

    # Run the test suite
    runner = unittest.TextTestRunner()
    result = runner.run(suite)

    # Set exit code based on test results
    if not result.wasSuccessful():
        sys.exit(1)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
