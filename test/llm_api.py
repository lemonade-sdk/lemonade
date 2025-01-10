import unittest
import shutil
import os
import urllib3
import platform
import zipfile
import requests
import logging
from turnkeyml.state import State
import turnkeyml.common.filesystem as fs
import turnkeyml.common.test_helpers as common
from lemonade.tools.huggingface_load import HuggingfaceLoad
from lemonade.tools.huggingface_bench import HuggingfaceBench
from lemonade.tools.mmlu import AccuracyMMLU
from lemonade.tools.humaneval import AccuracyHumaneval
from lemonade.tools.chat import LLMPrompt
from lemonade.tools.llamacpp import LoadLlamaCpp
from lemonade.tools.llamacpp_bench import LlamaCppBench
from lemonade.cache import Keys

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

ci_mode = os.getenv("LEMONADE_CI_MODE", False)

# Get cache directory from environment or create a new one
cache_dir = os.getenv('LEMONADE_CACHE_DIR')
if not cache_dir:
    cache_dir, _ = common.create_test_dir("lemonade_api")
    os.environ['LEMONADE_CACHE_DIR'] = cache_dir

logger.info(f"Using cache directory: {cache_dir}")

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


def download_llamacpp_binary():
    """Download the appropriate llama.cpp binary for the current platform"""
    logger.info("Starting llama.cpp binary download...")
    
    # Get latest release info
    releases_url = "https://api.github.com/repos/ggerganov/llama.cpp/releases/latest"
    try:
        response = requests.get(releases_url)
        response.raise_for_status()
        latest_release = response.json()
        logger.info(f"Found latest release: {latest_release.get('tag_name', 'unknown')}")
    except Exception as e:
        logger.error(f"Failed to fetch latest release info: {str(e)}")
        raise

    # Determine platform-specific binary pattern
    system = platform.system().lower()
    machine = platform.machine().lower()
    logger.info(f"Detected platform: {system} {machine}")
    
    if system == "windows":
        # Windows uses AVX2 by default
        asset_pattern = "win-avx2-x64"
    elif system == "linux":
        asset_pattern = "ubuntu-x64"
    else:
        error_msg = f"Unsupported platform: {system}"
        logger.error(error_msg)
        raise RuntimeError(error_msg)

    # Find matching asset
    matching_assets = [
        asset for asset in latest_release["assets"] 
        if (
            asset["name"].lower().startswith("llama-") and 
            asset_pattern in asset["name"].lower()
        )
    ]
    
    if not matching_assets:
        error_msg = (
            f"No matching binary found for {system} {machine}. "
            f"Looking for pattern: {asset_pattern}"
        )
        logger.error(error_msg)
        raise RuntimeError(error_msg)
    
    asset = matching_assets[0]
    logger.info(f"Found matching asset: {asset['name']}")
    
    # Create binaries directory
    binary_dir = os.path.join(cache_dir, "llama_cpp_binary")
    os.makedirs(binary_dir, exist_ok=True)
    logger.info(f"Created binary directory: {binary_dir}")
    
    # Download and extract
    zip_path = os.path.join(binary_dir, asset["name"])
    try:
        response = requests.get(asset["browser_download_url"])
        response.raise_for_status()
        
        with open(zip_path, "wb") as f:
            f.write(response.content)
        logger.info(f"Downloaded binary to: {zip_path}")

        with zipfile.ZipFile(zip_path, "r") as zip_ref:
            zip_ref.extractall(binary_dir)
        logger.info("Extracted binary archive")
    except Exception as e:
        logger.error(f"Failed to download or extract binary: {str(e)}")
        raise
    
    # Find the executable
    if system == "windows":
        executable = os.path.join(binary_dir, "llama-cli.exe")
    else:
        executable = os.path.join(binary_dir, "llama-cli")
        # Make executable on Linux
        os.chmod(executable, 0o755)
    
    if not os.path.exists(executable):
        error_msg = (
            f"Expected executable not found at {executable} after extraction. "
            f"Contents of {binary_dir}: {os.listdir(binary_dir)}"
        )
        logger.error(error_msg)
        raise RuntimeError(error_msg)
    
    logger.info(f"Successfully prepared executable at: {executable}")
    return executable


class TestLlamaCpp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """Download llama.cpp binary once for all tests"""
        logger.info("Setting up TestLlamaCpp class...")
        try:
            cls.executable = download_llamacpp_binary()
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
            model_url = f"https://huggingface.co/{cls.model_name}/resolve/main/{cls.model_file}"
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
            executable=self.executable,
            model_binary=self.model_path,
            context_size=512,
            threads=1
        )
        
        self.assertIsNotNone(state.model)

    def test_002_generate_text(self):
        """Test text generation with llama.cpp"""
        state = LoadLlamaCpp().run(
            self.state,
            executable=self.executable,
            model_binary=self.model_path
        )
        
        prompt = "What is the capital of France?"
        state = LLMPrompt().run(state, prompt=prompt, max_new_tokens=20)
        
        self.assertIsNotNone(state.response)
        self.assertGreater(len(state.response), len(prompt))

    def test_003_benchmark(self):
        """Test benchmarking with llama.cpp"""
        state = LoadLlamaCpp().run(
            self.state,
            executable=self.executable,
            model_binary=self.model_path
        )
        
        # Use longer output tokens to ensure we get valid performance metrics
        state = LlamaCppBench().run(
            state,
            iterations=2,
            warmup_iterations=1,
            output_tokens=128,
            prompt="Hello, I am a test prompt that is long enough to get meaningful metrics."
        )
        
        stats = fs.Stats(state.cache_dir, state.build_name).stats
        
        # Check if we got valid metrics
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
            k_samples=1,        # Single attempt per problem
            timeout=30.0
        )

        # Verify results
        stats = fs.Stats(state.cache_dir, state.build_name).stats
        assert "humaneval_pass@1" in stats, "HumanEval pass@1 metric not found"
        assert isinstance(stats["humaneval_pass@1"], (int, float)), "HumanEval pass@1 metric should be numeric"

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
    
    # Create test suite with all test classes
    suite = unittest.TestSuite()
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(Testing))
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestLlamaCpp))
    
    # Run the test suite
    runner = unittest.TextTestRunner()
    runner.run(suite)
