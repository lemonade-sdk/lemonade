import unittest
import shutil
import os
import sys
import platform
import zipfile
import logging
import urllib3
import requests
from lemonade.state import State
import lemonade.common.filesystem as fs
import lemonade.common.test_helpers as common
import lemonade.common.build as build
from lemonade.tools.huggingface.load import HuggingfaceLoad
from lemonade.tools.huggingface.bench import HuggingfaceBench
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.humaneval import AccuracyHumaneval
from lemonade.tools.accuracy import LMEvalHarness
from lemonade.tools.prompt import LLMPrompt
from lemonade.tools.llamacpp.load import LoadLlamaCpp
from lemonade.tools.llamacpp.bench import LlamaCppBench
from lemonade.tools.llamacpp.utils import (
    install_llamacpp,
    get_llama_cli_exe_path,
    get_llama_folder_path,
)
from lemonade.cache import Keys

# Configure logging
logging.basicConfig(
    level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Use None as the default value for environment variables
ci_mode = os.getenv("LEMONADE_CI_MODE", None)

# Define cache_dir and corpus_dir at the module level
cache_dir = None
corpus_dir = None


def download_llamacpp_binary(backend: str = "vulkan"):
    """Download the appropriate llama.cpp binary for the current platform"""
    # Install llamacpp binary using the official function
    install_llamacpp(backend)

    # Get the executable path
    executable = get_llama_cli_exe_path(backend)

    # Make executable on Linux
    if system != "windows":
        os.chmod(executable, 0o755)

    # Find shared libraries directory for Linux
    lib_dir = None
    system = platform.system().lower()
    if system != "windows":
        # Get the binary directory
        binary_dir = get_llama_folder_path(backend)

        # Look for libllama.so in the extracted directory
        for root, dirs, files in os.walk(binary_dir):
            if any(f.startswith("libllama.so") for f in files):
                lib_dir = root
                logger.info(f"Found shared libraries in: {lib_dir}")
                break

    logger.info(f"Successfully prepared executable at: {executable}")
    if lib_dir:
        logger.info(f"Shared libraries directory: {lib_dir}")

    return executable, lib_dir


class TestLlamaCpp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """Download llama.cpp binary once for all tests"""
        logger.info("Setting up TestLlamaCpp class...")
        try:
            cls.executable, cls.lib_dir = download_llamacpp_binary()
        except Exception as e:
            error_msg = f"Failed to download llama.cpp binary: {str(e)}"
            logger.error(error_msg)
            raise unittest.SkipTest(error_msg)

        # Use a small GGUF model for testing
        cls.model_name = "Qwen/Qwen2.5-0.5B-Instruct-GGUF"
        cls.model_file = "qwen2.5-0.5b-instruct-fp16.gguf"
        logger.info(f"Using test model: {cls.model_name}/{cls.model_file}")

        # Download the model file
        try:
            model_url = (
                f"https://huggingface.co/{cls.model_name}/resolve/main/{cls.model_file}"
            )
            cls.model_path = os.path.join(cache_dir, cls.model_file)

            if not os.path.exists(cls.model_path):
                logger.info(f"Downloading model from: {model_url}")
                response = requests.get(model_url)
                response.raise_for_status()
                with open(cls.model_path, "wb") as f:
                    f.write(response.content)
                logger.info(f"Model downloaded to: {cls.model_path}")
            else:
                logger.info(f"Using existing model at: {cls.model_path}")
        except Exception as e:
            error_msg = f"Failed to download test model: {str(e)}"
            logger.error(error_msg)
            raise unittest.SkipTest(error_msg)

    def setUp(self):
        self.state = State(
            cache_dir=cache_dir,
            build_name="test_llamacpp",
        )

    def test_001_load_model(self):
        """Test loading a model with llama.cpp"""
        state = LoadLlamaCpp().run(
            self.state,
            self.model_name + ":" + self.model_file,
            context_size=512,
            threads=1,
        )

        self.assertIsNotNone(state.model)

    def test_002_generate_text(self):
        """Test text generation with llama.cpp"""
        state = LoadLlamaCpp().run(
            self.state,
            self.model_name + ":" + self.model_file,
        )

        prompt = "What is the capital of France?"
        state = LLMPrompt().run(state, prompt=prompt, max_new_tokens=20)

        self.assertIsNotNone(state.response)
        self.assertGreater(len(state.response), 0, state.response)

    def test_003_benchmark(self):
        """Test benchmarking with llama.cpp"""
        state = LoadLlamaCpp().run(
            self.state,
            self.model_name + ":" + self.model_file,
        )

        # Use longer output tokens to ensure we get valid performance metrics
        state = LlamaCppBench().run(
            state,
            iterations=2,
            warmup_iterations=1,
            output_tokens=128,
            prompts=[
                "Hello, I am a test prompt that is long enough to get meaningful metrics."
            ],
        )

        # Check if we got valid metrics
        stats = fs.Stats(state.cache_dir, state.build_name).stats
        self.assertIn(Keys.TOKEN_GENERATION_TOKENS_PER_SECOND, stats)
        self.assertIn(Keys.SECONDS_TO_FIRST_TOKEN, stats)


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

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert len(stats["response"]) > 0, stats["response"]

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
        assert stats[f"mmlu_{subject[0]}_accuracy"] >= 0

    def test_003_accuracy_humaneval(self):
        """Test HumanEval benchmarking with known model"""
        checkpoint = "facebook/opt-125m"

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        # Enable code evaluation for HumanEval
        os.environ["HF_ALLOW_CODE_EVAL"] = "1"

        state = HuggingfaceLoad().run(state, input=checkpoint)
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

    def test_004_accuracy_lmeval(self):
        """Test lm-eval-harness benchmarking with known model"""
        checkpoint = "facebook/opt-125m"

        state = State(
            cache_dir=cache_dir,
            build_name="test_lmeval",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        state = LMEvalHarness().run(
            state,
            task="mmlu_abstract_algebra",
            limit=1,
            num_fewshot=0,
        )

        # Verify results
        stats = fs.Stats(state.cache_dir, state.build_name).stats

        # Check if any lm_eval stats were saved
        lm_eval_stats = [k for k in stats.keys() if k.startswith("lm_eval_")]
        assert len(lm_eval_stats) > 0, "No lm-eval-harness metrics found in stats"

        # Check for specific metrics that should exist
        for metric_name in lm_eval_stats:
            if metric_name.endswith("_units"):
                # Units stats should be strings (e.g., "%")
                assert isinstance(
                    stats[metric_name], str
                ), f"{metric_name} should be a string (units)"
            else:
                # All other lm_eval stats should be numeric
                assert isinstance(
                    stats[metric_name], (int, float)
                ), f"{metric_name} should be numeric"

    def test_004_huggingface_bench(self):
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

    def test_005_prompt_from_file(self):
        """
        Test the LLM Prompt tool capability to load prompt from a file
        """

        checkpoint = "facebook/opt-125m"
        prompt_str = "Who is Humpty Dumpty?"

        prompt_path = os.path.join(corpus_dir, "prompt.txt")
        with open(prompt_path, "w", encoding="utf-8") as f:
            f.write(prompt_str)

        llm_prompt_args = ["-p", prompt_path, "--max-new-tokens", "15"]

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        llm_prompt_kwargs = LLMPrompt().parse(state, llm_prompt_args).__dict__
        state = LLMPrompt().run(state, **llm_prompt_kwargs)

        stats = fs.Stats(state.cache_dir, state.build_name).stats

        assert len(stats["response"]) > 0, stats["response"]
        assert stats["prompt"] == prompt_str, f"{stats['prompt']} {prompt_str}"

    def test_006_multiple_prompt_responses(self):
        """
        Test the LLM Prompt tool capability to run multiple inferences on the same prompt
        """

        checkpoint = "facebook/opt-125m"
        prompt_str = "Who is Humpty Dumpty?"
        n_trials = 2

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        state = LLMPrompt().run(
            state, prompt=prompt_str, max_new_tokens=15, n_trials=n_trials
        )

        stats = fs.Stats(state.cache_dir, state.build_name).stats

        # Check that two responses were generated
        assert (
            isinstance(stats["response"], list) and len(stats["response"]) == n_trials
        ), stats["response"]
        assert (
            isinstance(stats["response_tokens"], list)
            and len(stats["response_tokens"]) == n_trials
        ), stats["response_tokens"]
        # Check that histogram figure was generated
        assert os.path.exists(
            os.path.join(
                build.output_dir(state.cache_dir, state.build_name),
                "response_lengths.png",
            )
        )

    def test_007_huggingface_multiple_bench(self):
        # Benchmark OPT
        checkpoint = "facebook/opt-125m"

        state = State(
            cache_dir=cache_dir,
            build_name="test",
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        state = HuggingfaceBench().run(
            state, iterations=20, prompts=["word " * 30, "word " * 62]
        )

        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert len(stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND]) == 2
        assert all(x > 0 for x in stats[Keys.TOKEN_GENERATION_TOKENS_PER_SECOND])


class TestHfLogprobs(unittest.TestCase):
    """
    Test the compute_logprobs functionality in Huggingface implementation.
    """

    def setUp(self) -> None:
        # Use a unique build name for each test to avoid conflicts
        self.build_name = f"test_hf_logprobs"

    def test_008_compute_logprobs_completion(self):
        """Test compute_logprobs with a specific completion"""
        checkpoint = "facebook/opt-125m"

        state = State(
            cache_dir=cache_dir,
            build_name=self.build_name,
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        # Ensure model has compute_logprobs method
        self.assertTrue(
            hasattr(state.model, "compute_logprobs"),
            "Model should have compute_logprobs method",
        )

        # Test with a simple prompt
        text = "The capital of France is Paris"

        text_offset, token_logprobs, tokens, top_logprobs = (
            state.model.compute_logprobs(
                text=text, tokenizer=state.tokenizer, logprobs=5
            )
        )

        # Verify we got valid outputs
        self.assertIsNotNone(text_offset)
        self.assertIsNotNone(token_logprobs)
        self.assertIsNotNone(tokens)
        self.assertIsNotNone(top_logprobs)

        # Check the content of the output
        self.assertEqual(len(tokens), len(token_logprobs))
        self.assertEqual(len(tokens), len(text_offset))
        self.assertEqual(len(tokens), len(top_logprobs))

    def test_009_compute_logprobs_echo_parameter(self):
        """
        Test compute_logprobs with echo parameter controlling prompt token inclusion.
        """
        checkpoint = "facebook/opt-125m"

        state = State(
            cache_dir=cache_dir,
            build_name=self.build_name,
        )

        state = HuggingfaceLoad().run(state, input=checkpoint)
        # Define test inputs
        prefix = "This is a test prompt."
        completion = "This is the completion."
        full_text = prefix + " " + completion

        # First encode the text to get token counts
        tokens = state.tokenizer(prefix).input_ids
        prompt_length = len(tokens)

        # Test with echo=False using prompt_length
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
        self.assertEqual(len(tokens_no_echo) + prompt_length, len(tokens_with_echo))

        # Verify echo=True includes all tokens
        self.assertGreaterEqual(len(tokens_with_echo), prompt_length)

        # Test edge case - no prompt tokens
        zero_prompt_text = "Only completion tokens."
        zero_offset, zero_logprobs, zero_tokens, _ = state.model.compute_logprobs(
            text=zero_prompt_text,
            tokenizer=state.tokenizer,
            prompt_length=0,  # No prompt tokens
            logprobs=5,
            echo=False,
        )

        # All tokens should be included when prompt_length=0
        all_tokens = state.model.tokenizer(zero_prompt_text).input_ids
        self.assertEqual(len(zero_tokens), len(all_tokens))


class TestSystemInfoAPI(unittest.TestCase):
    """
    Test the system information API functions.
    """

    def test_001_get_system_info_basic(self):
        """
        Test basic system info functionality.
        """
        from lemonade.api import get_system_info

        system_info = get_system_info()

        # Check it returns a dictionary
        self.assertIsInstance(system_info, dict)

        # Check required keys exist in default (non-verbose) mode
        required_keys = ["OS Version", "Processor", "Physical Memory", "Devices"]
        for key in required_keys:
            self.assertIn(key, system_info)

        # Basic validation
        self.assertIsInstance(system_info["OS Version"], str)
        self.assertIsInstance(system_info["Devices"], dict)

    def test_001_get_system_info_verbose(self):
        """
        Test verbose system info functionality.
        """
        from lemonade.api import get_system_info

        system_info = get_system_info(verbose=True)

        # Check it returns a dictionary
        self.assertIsInstance(system_info, dict)

        # Check required keys exist in verbose mode
        required_keys = ["OS Version", "Devices", "Python Packages"]
        for key in required_keys:
            self.assertIn(key, system_info)

        # Basic validation
        self.assertIsInstance(system_info["OS Version"], str)
        self.assertIsInstance(system_info["Devices"], dict)
        self.assertIsInstance(system_info["Python Packages"], list)
        self.assertGreater(len(system_info["Python Packages"]), 0)

    def test_002_get_device_info_basic(self):
        """
        Test basic device info functionality.
        """
        from lemonade.api import get_device_info

        device_info = get_device_info()

        # Check it returns a dictionary
        self.assertIsInstance(device_info, dict)

        # Check required device types exist
        required_devices = ["cpu", "amd_igpu", "amd_dgpu", "npu"]
        for device in required_devices:
            self.assertIn(device, device_info)

        # Check CPU has proper structure
        cpu_info = device_info["cpu"]
        self.assertIsInstance(cpu_info, dict)
        self.assertIn("available", cpu_info)


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
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(Testing))
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestHfLogprobs))
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestLlamaCpp))
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestSystemInfoAPI))

    # Run the test suite
    runner = unittest.TextTestRunner()
    result = runner.run(suite)

    # Set exit code based on test results
    if not result.wasSuccessful():
        sys.exit(1)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
