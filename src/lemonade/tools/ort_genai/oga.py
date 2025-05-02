# onnxruntime_genai is not lint-friendly yet and PyLint can't
# find any of the class methods
# pylint: disable=no-member
#
# Model builder constraints:
#   11/10/24 Need transformers <4.45.0 OR onnxruntime-genai 0.5.0 (which must be built from source)
#   (transformers v4.45 changes the format of the tokenizer.json file which will be supported in
#   onnxruntime-genai 0.5)
#

import argparse
import os
import time
import json
import shutil
import logging
from fnmatch import fnmatch
from queue import Queue
import subprocess
from packaging.version import Version
from huggingface_hub import snapshot_download
import onnxruntime_genai as og
import onnxruntime_genai.models.builder as model_builder
from transformers import AutoTokenizer
from turnkeyml.state import State
from turnkeyml.tools import FirstTool
import turnkeyml.common.status as status
import turnkeyml.common.printing as printing
from lemonade.tools.huggingface_load import get_base_model, is_offline
from lemonade.tools.adapter import (
    ModelAdapter,
    TokenizerAdapter,
    PassthroughTokenizerResult,
)
from lemonade.cache import Keys
from lemonade_install.install import (
    get_ryzen_ai_version_info,
    get_oga_npu_dir,
    get_oga_hybrid_dir,
)


# ONNX Runtime GenAI models will be cached in this subfolder of the lemonade cache folder
oga_models_path = "oga_models"

# ONNX Runtime GenAI model builder tool uses this subfolder of the lemonade cache as its cache
oga_model_builder_cache_path = "model_builder"

# Mapping from processor to execution provider, used in pathnames and by model_builder
execution_providers = {
    "cpu": "cpu",
    "npu": "npu",
    "igpu": "dml",
    "hybrid": "hybrid",
    "cuda": "cuda",
}


class OrtGenaiTokenizer(TokenizerAdapter):
    def __init__(self, model: og.Model, hf_tokenizer: AutoTokenizer):
        super().__init__(hf_tokenizer)
        # Initialize OGA tokenizer
        self.tokenizer = og.Tokenizer(model)

        # Placeholder value since some code will try to query it
        # If we actually need this to return a proper value, then
        # og.GeneratorParams.eos_token_id has it
        self.eos_token_id = None

    def __call__(self, prompt: str, return_tensors="np"):
        tokens = self.tokenizer.encode(prompt)
        return PassthroughTokenizerResult(tokens)

    # pylint: disable=unused-argument
    def decode(self, response, skip_special_tokens=True) -> str:
        return self.tokenizer.decode(response)


class OrtGenaiStreamer:
    def __init__(self, tokenizer: OrtGenaiTokenizer, timeout=None):
        self.tokenizer = tokenizer
        self.text_queue = Queue()
        self.stop_signal = None
        self.timeout = timeout

    def add_text(self, text: str):
        self.text_queue.put(text, timeout=self.timeout)

    def done(self):
        self.text_queue.put(self.stop_signal, timeout=self.timeout)

    def __iter__(self):
        return self

    def __next__(self):
        value = self.text_queue.get(timeout=self.timeout)
        if value == self.stop_signal:
            raise StopIteration()
        else:
            return value


class OrtGenaiModel(ModelAdapter):

    def __init__(self, input_folder):
        super().__init__()
        self.model = og.Model(input_folder)
        self.type = "ort-genai"
        self.config = self.load_config(input_folder)

    def load_config(self, input_folder):
        config_path = os.path.join(input_folder, "genai_config.json")
        if os.path.exists(config_path):
            with open(config_path, "r", encoding="utf-8") as f:
                return json.load(f)
        return None

    def generate(
        self,
        input_ids,
        max_new_tokens=512,
        min_new_tokens=0,
        do_sample=True,
        top_k=50,
        top_p=1.0,
        temperature=0.7,
        streamer: OrtGenaiStreamer = None,
        pad_token_id=None,
        stopping_criteria=None,
        max_length=None,
    ):
        params = og.GeneratorParams(self.model)

        # There is a breaking API change in OGA 0.6.0
        # Determine whether we should use the old or new APIs
        # This also supports 0.6.0.dev0, which evaluates to less than 0.6.0 in Version
        use_oga_post_6_api = (
            Version(og.__version__) >= Version("0.6.0") or "0.6.0" in og.__version__
        )
        use_oga_pre_6_api = not use_oga_post_6_api

        if pad_token_id:
            params.pad_token_id = pad_token_id

        # Handle max_length and max_new_tokens
        if max_length and max_new_tokens:
            logging.warning(
                "Both max_length and max_new_tokens were provided. "
                "max_length will take precedence. "
                "When setting max_length, please explicitly set max_new_tokens to None."
            )
        max_length_to_use = None
        if max_length:
            max_length_to_use = max_length
        elif max_new_tokens:
            max_length_to_use = len(input_ids) + max_new_tokens

        min_length = len(input_ids) + min_new_tokens

        if use_oga_pre_6_api:
            params.input_ids = input_ids

        if self.config and "search" in self.config:
            search_config = self.config["search"]
            params.set_search_options(
                do_sample=search_config.get("do_sample", do_sample),
                top_k=search_config.get("top_k", top_k),
                top_p=search_config.get("top_p", top_p),
                temperature=search_config.get("temperature", temperature),
                max_length=max_length_to_use,
                min_length=min_length,
                early_stopping=search_config.get("early_stopping", False),
                length_penalty=search_config.get("length_penalty", 1.0),
                num_beams=search_config.get("num_beams", 1),
                num_return_sequences=search_config.get("num_return_sequences", 1),
                repetition_penalty=search_config.get("repetition_penalty", 1.0),
                past_present_share_buffer=search_config.get(
                    "past_present_share_buffer", True
                ),
                # Make sure that results do not vary across laptops
                # by default, random_seed=-1 causes different laptops to give
                # different results
                random_seed=1,
                # Not currently supported by OGA
                # diversity_penalty=search_config.get('diversity_penalty', 0.0),
                # no_repeat_ngram_size=search_config.get('no_repeat_ngram_size', 0),
            )
        else:
            params.set_search_options(
                do_sample=do_sample,
                top_k=top_k,
                top_p=top_p,
                temperature=temperature,
                max_length=max_length_to_use,
                min_length=min_length,
            )
        params.try_graph_capture_with_max_batch_size(1)

        generator = og.Generator(self.model, params)

        if streamer is None:
            prompt_start_time = time.perf_counter()
            if use_oga_post_6_api:
                generator.append_tokens(input_ids)
            if use_oga_pre_6_api:
                generator.compute_logits()
            generator.generate_next_token()
            prompt_end_time = time.perf_counter()

            self.time_to_first_token = prompt_end_time - prompt_start_time

            if max_new_tokens > 1:

                token_gen_times = []
                while not generator.is_done():
                    token_gen_start_time = time.perf_counter()
                    if use_oga_pre_6_api:
                        generator.compute_logits()
                    generator.generate_next_token()
                    token_gen_end_time = time.perf_counter()

                    token_gen_times.append(token_gen_end_time - token_gen_start_time)

                if token_gen_times:
                    # List will be empty if we generated 1 or 0 tokens, and we don't
                    # want a divide-by-zero error in those cases
                    avg_token_gen_latency_s = sum(token_gen_times) / len(
                        token_gen_times
                    )
                    self.tokens_per_second = 1 / avg_token_gen_latency_s

            return [generator.get_sequence(0)]
        else:
            if use_oga_post_6_api:
                generator.append_tokens(input_ids)
            tokenizer_stream = streamer.tokenizer.tokenizer.create_stream()

            stop_early = False

            while not generator.is_done() and not stop_early:
                if use_oga_pre_6_api:
                    generator.compute_logits()
                generator.generate_next_token()

                new_token = generator.get_next_tokens()[0]
                new_text = tokenizer_stream.decode(new_token)

                streamer.add_text(new_text)

                if stopping_criteria is not None:
                    if stopping_criteria[0].stop_event.is_set():
                        stop_early = True

            streamer.done()

    def _model_call(self, input_ids):
        """
        Run the model on input_ids and get logits.

        This method directly accesses model logits rather than using the full generate pipeline for
        several important reasons:
        1. Purpose: We need raw logits from a single forward pass, while generate() is optimized for
           producing multiple tokens through iterative inference
        2. Efficiency: Direct access is more efficient for logprob calculations with no
           sampling overhead
        3. Precision: Logprob calculations require exact control over input-to-output mapping
        4. Consistency: Similar approach used in both HF and OGA implementations

        Args:
            input_ids: Input token IDs

        Returns:
            Logits for each token in the sequence
        """
        import torch

        # Setup generator params
        params = og.GeneratorParams(self.model)

        # Configure for a simple forward pass
        params.set_search_options(
            do_sample=False,
            temperature=0.0,
            max_length=len(input_ids),
        )

        # Initialize generator
        generator = og.Generator(self.model, params)

        # Feed tokens to model based on API version
        generator.append_tokens(input_ids)

        # Extract logits - this returns a list of logits tensors
        logits = generator.get_output("logits")

        # Convert to torch tensor for easier processing
        return torch.tensor(logits[0])

    def _select_cont_toks(self, logits, context_len, continuation_tokens):
        """
        Select and process logits for continuation tokens.

        Args:
            logits: Full sequence logits
            context_len: Length of context tokens
            continuation_tokens: List or tensor of continuation token IDs

        Returns:
            Log probabilities for continuation tokens
        """
        import torch

        # Extract relevant logits for continuation prediction (shift by one)
        cont_logits = logits[
            context_len - 1 : context_len - 1 + len(continuation_tokens)
        ]

        # Convert to torch tensors if needed
        if not isinstance(continuation_tokens, torch.Tensor):
            continuation_tokens = torch.tensor(continuation_tokens, dtype=torch.long)

        # Apply log softmax to get log probabilities
        log_probs = torch.log_softmax(cont_logits, dim=-1)

        # Get log probs for the specific continuation tokens
        token_log_probs = torch.gather(
            log_probs, 1, continuation_tokens.unsqueeze(-1)
        ).squeeze(-1)

        return token_log_probs

    def compute_logprobs(
        self, text, tokenizer, prompt_length=None, logprobs=None, echo=False
    ):
        """
        Compute log probabilities for all tokens in the given text.

        Args:
            text: The full text to analyze (e.g., prompt + completion)
            prompt_length: Number of tokens in the prompt. If provided and echo=False,
                only completion tokens after this position will be returned.
            logprobs: If not None, return log probabilities. Value indicates how many top
                alternatives to return. If True but not an integer, defaults to 5 alternatives.
            echo: If True, include logprobs for prompt tokens. If False, only return logprobs
                for completion tokens.

        Returns:
            - text_offset: Character offsets for each token in the text
            - token_logprobs: Log probability for each token
            - tokens: The actual tokens used
            - top_logprobs: Top alternative log probabilities for each position
        """
        import torch

        if tokenizer is None:
            raise ValueError("Tokenizer is required for logprob calculation")

        # Encode the full text
        tokens = tokenizer(text).input_ids  # pylint: disable=E1102

        # Track character offsets for each token
        text_offset = []
        start_idx = 0

        token_strings = []
        for token_id in tokens:
            token_str = tokenizer.decode([token_id])
            token_strings.append(token_str)

            # Calculate character offsets for tokens - handles cases where tokens
            # may not directly match in the original text due to encoding differences,
            # special characters, or tokenization artifacts
            try:
                pos = text[start_idx:].find(token_str)
                if pos != -1:
                    text_offset.append(start_idx + pos)
                    start_idx += pos + len(token_str)
                else:
                    text_offset.append(start_idx)
            except (TypeError, ValueError, UnicodeError):
                # Fallback to current position when matching fails due to encoding issues
                text_offset.append(start_idx)

        # Get logits from model
        logits = self._model_call(tokens)

        # Calculate log probabilities for each token
        all_log_probs = torch.log_softmax(logits, dim=-1)

        # The first token doesn't have a conditional probability
        # For tokens after the first, get the predicted probability
        token_log_probs = []
        top_logprobs_list = []

        # For each position, get the actual token probability and top alternatives
        for i in range(len(tokens)):
            # Get previous token position logits
            if i > 0:  # First token has no preceding context
                prev_logits = all_log_probs[i - 1]
                curr_token_id = tokens[i]
                # Get probability of the actual token that appeared
                token_logprob = prev_logits[curr_token_id].item()
                token_log_probs.append(token_logprob)

                # Get top-k alternatives if requested
                if logprobs is not None:
                    num_alternatives = logprobs if isinstance(logprobs, int) else 5
                    topk_values, topk_indices = torch.topk(
                        prev_logits, min(num_alternatives, prev_logits.size(-1))
                    )

                    # Create dictionary of token: logprob
                    position_logprobs = {}
                    for val, idx in zip(topk_values.tolist(), topk_indices.tolist()):
                        token_str = tokenizer.decode([idx])
                        position_logprobs[token_str] = val

                    top_logprobs_list.append(position_logprobs)
            else:
                # For the first token, we don't have a conditional probability
                token_log_probs.append(None)
                top_logprobs_list.append({})

        # If we don't want to echo prompt tokens, filter them out
        if not echo and prompt_length is not None:
            # Ensure prompt_length is within bounds
            prompt_length = min(prompt_length, len(tokens))

            # Filter results to only include completion tokens
            if prompt_length < len(tokens):
                filtered_text_offset = text_offset[prompt_length:]
                filtered_token_logprobs = token_log_probs[prompt_length:]
                filtered_tokens = token_strings[prompt_length:]
                filtered_top_logprobs = top_logprobs_list[prompt_length:]

                return (
                    filtered_text_offset,
                    filtered_token_logprobs,
                    filtered_tokens,
                    filtered_top_logprobs,
                )
            else:
                # No completion tokens
                return [], [], [], []

        return text_offset, token_log_probs, token_strings, top_logprobs_list


class OgaLoad(FirstTool):
    """
    Tool that loads an LLM in OnnxRuntime-GenAI for use with CPU or DirectML execution providers.

    Input: path to a checkpoint.
        Supported choices for cpu and igpu from HF model repository:
            LLM models on Huggingface supported by model_builder.  See documentation
            (https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/ort_genai_igpu.md)
            for supported models.
        Supported choices for npu from HF model repository:
            Models on Hugging Face that follow the "amd/**-onnx-ryzen-strix" pattern
        Local models for cpu, igpu, or npu:
            The specified checkpoint is converted to a local path, via mapping to lower case
            and replacing '/' with '_'.  If this model already exists in the 'models' folder
            of the lemonade cache and if it has a subfolder <device>-<dtype>, then this model
            will be used.  If the --force flag is used and the model is built with model_builder,
            then it will be rebuilt.



    Output:
        state.model: handle to a Huggingface-style LLM loaded on DirectML device
        state.tokenizer = Huggingface-style LLM tokenizer instance
        state.dtype = data type of the model on DirectML device
        state.checkpoint = name of the checkpoint used to load state.model

    Note: This tool expects the onnxruntime-genai-directml library to be pre-installed.
            If that library is not installed, this tool will not load.
    """

    unique_name = "oga-load"

    def __init__(self):
        super().__init__(monitor_message="Loading OnnxRuntime-GenAI model")

        self.status_stats = [
            Keys.DTYPE,
            Keys.DEVICE,
            Keys.LOCAL_MODEL_FOLDER,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load model in onnxruntime-genai (OGA)",
            add_help=add_help,
        )

        parser.add_argument(
            "-ip",
            "--input_path",
            default="",
            help="the local huggingface model in your disk",
        )

        parser.add_argument(
            "-d",
            "--device",
            choices=["igpu", "npu", "cpu", "hybrid", "cuda"],
            default="igpu",
            help="Which device to load the model on to (default: igpu)",
        )

        parser.add_argument(
            "--dtype",
            choices=["int4", "fp16", "fp32"],
            required=True,
            help="Data type to load the model in",
        )

        parser.add_argument(
            "--int4-block-size",
            default=None,
            help="Specify the block_size for int4 quantization.",
            choices=[16, 32, 64, 128, 256],
            type=int,
        )

        parser.add_argument(
            "--force",
            action="store_true",
            help="Forces downloading of Hugging-Face model again (if changed).  Additionally for"
            " cpu and igpu devices only, forces model_builder to run again on the HF model"
            " (changed or not).",
        )

        parser.add_argument(
            "--download-only",
            action="store_true",
            help="Download the model if needed, but don't load it",
        )

        parser.add_argument(
            "--trust-remote-code",
            action="store_true",
            help="Set this flag to use models whose code is on the Hugging Face hub rather "
            "than natively in the OnnxRuntime Gen AI libraries.  Please review the model code "
            "in advance as this is a security risk.",
        )

        parser.add_argument(
            "--subfolder",
            default=None,
            help="Subfolder where model is located <LEMONADE CACHE>/oga_models/<MODELNAME>"
            "/<SUBFOLDER>, default is <EP for device>-<dtype>.  The EPs are: "
            f'{", ".join([value + " for " + key for key, value in execution_providers.items()])}.',
        )

        return parser

    @staticmethod
    def _validate_model_configuration(device, dtype, checkpoint):
        """
        Validate if the device, dtype, and checkpoint combination are consistent with
        HuggingFace checkpoint naming conventions and specifically for AMD models for NPU
        and hybrid flows.

        Returns True if device, dtype, and model are consistent.
        """
        hf_supported_models = {
            "cpu": {"int4": "*/*", "fp32": "*/*"},
            "igpu": {"int4": "*/*", "fp16": "*/*"},
            "npu": {"int4": "*/*"},
            "hybrid": {"int4": "*/*"},
            "cuda": {"int4": "*/*", "fp16": "*/*"},
        }

        hf_supported = (
            device in hf_supported_models
            and dtype in hf_supported_models[device]
            and fnmatch(checkpoint, hf_supported_models[device][dtype])
        )
        return hf_supported

    @staticmethod
    def _setup_model_paths(
        state, checkpoint, device, dtype, subfolder, int4_block_size
    ):
        """
        Determines and returns the following model path information for models produced by OGA
        model builder:

           full_model_path - Full path to where the OGA model files are stored.
           oga_models_subfolder - The subfolder of the oga_models folder where the model files
                are stored.  (<full_model_path> = <oga_models>/<oga_models_subfolder>)
                This subfolder is usually
                  <checkpoint_string>/<device>-<dtype>[-block-<int4_block_size]>
                but the if the argument subfolder is not None it will override the latter part
                of this path.
           model_exists_locally - True if full_model_path is a folder that contains files

        Note: Model files already in ONNX format on Hugging Face will be stored in the
            Hugging Face cache, not this folder.  The <oga_models> folder contains model
            files that have locally been quantized/converted to OGA format and any other
            models that have been manually added by the user.
        """
        if subfolder is None:
            subfolder = f"{execution_providers[device]}-{dtype}"
            subfolder += (
                f"-block-{int4_block_size}"
                if dtype == "int4" and int4_block_size is not None
                else ""
            )

        # First, check in the lemonade oga_models cache
        oga_models_subfolder = os.path.join(
            checkpoint.replace("/", "_").lower(), subfolder
        )
        full_model_path = os.path.join(
            state.cache_dir, oga_models_path, oga_models_subfolder
        )
        model_exists_locally = os.path.isdir(full_model_path) and os.listdir(
            full_model_path
        )

        # If not found in lemonade cache, check in Hugging Face cache
        if not model_exists_locally:
            try:
                snapshot_path = snapshot_download(
                    repo_id=checkpoint,
                    local_files_only=True,
                )

                # Check if the snapshot contains ONNX files
                if os.path.isdir(snapshot_path) and os.listdir(snapshot_path):
                    is_onnx_model = any(
                        filename.endswith(".onnx")
                        for filename in os.listdir(snapshot_path)
                    )

                    if is_onnx_model:
                        # If the model is in HF cache and has ONNX files, use it
                        full_model_path = snapshot_path
                        model_exists_locally = True
                        printing.log_info(
                            f"Found ONNX model in Hugging Face cache: {full_model_path}"
                        )
            except Exception as e:  # pylint: disable=broad-exception-caught
                # Log any errors but continue with the original path
                printing.log_info(f"Error checking Hugging Face cache: {e}")

        return full_model_path, model_exists_locally

    @staticmethod
    def _update_hybrid_custom_ops_library_path(full_model_path):
        """
        Modifies the genai_config.json file in the hybrid model folder to set the custom_ops_library
        path to the location of the onnx_custom_ops.dll in the current environment.
        This is needed for hybrid inference.
        """
        oga_path, version = get_oga_hybrid_dir()

        if "1.3.0" in version:
            custom_ops_path = os.path.join(
                oga_path,
                "onnx_utils",
                "bin",
                "onnx_custom_ops.dll",
            )
        else:
            custom_ops_path = os.path.join(oga_path, "libs", "onnx_custom_ops.dll")

        # Insert the custom_ops_path into the model config file
        config_path = os.path.join(full_model_path, "genai_config.json")
        if os.path.exists(config_path):
            with open(config_path, "r", encoding="utf-8") as f:
                config = json.load(f)

            if (
                "model" in config
                and "decoder" in config["model"]
                and "session_options" in config["model"]["decoder"]
            ):
                config["model"]["decoder"]["session_options"][
                    "custom_ops_library"
                ] = custom_ops_path

            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(config, f, indent=4)

        else:
            printing.log_info(
                f"Model's `genai_config.json` not found in {full_model_path}"
            )

    @staticmethod
    def _is_preoptimized_model(input_model_path):
        """
        Checks if the 'custom_ops_library' field exists in the genai_config.json file
        to determine if this is a pre-optimized model for hybrid as well
        as NPU only.

        Args:
            input_model_path (str): Path to the input model directory.

        Returns:
            bool: True if 'custom_ops_library' exists, False otherwise.
        """
        config_path = os.path.join(input_model_path, "genai_config.json")
        if not os.path.exists(config_path):
            printing.log_info(f"Model's `genai_config.json` not found in {config_path}")
            return False

        with open(config_path, "r", encoding="utf-8") as f:
            config = json.load(f)
        if (
            "model" in config
            and "decoder" in config["model"]
            and "session_options" in config["model"]["decoder"]
        ):
            return "custom_ops_library" in config["model"]["decoder"]["session_options"]
        return False

    @staticmethod
    def _download_and_build_safetensors_model(
        checkpoint, device, dtype, full_model_path, int4_block_size, input_path, state
    ):
        """
        Uses OGA model builder to quantize safetensors format model and convert to ONNX
        format.  The model files are saved to the full_model_path folder.
        """
        printing.log_info(f"Building {checkpoint} for {device} using {dtype}")
        extra_options = {}
        if int4_block_size is not None:
            extra_options["int4-block-size"] = int4_block_size
        try:
            model_builder.create_model(
                checkpoint,
                input_path,
                full_model_path,
                dtype,
                execution_providers[device],
                os.path.join(state.cache_dir, oga_model_builder_cache_path),
                **extra_options,
            )
        except NotImplementedError as e:
            raise NotImplementedError("[Model builder] " + str(e)) from e
        except OSError as e:
            raise ValueError("[Model builder] " + str(e)) from e

        return full_model_path

    @staticmethod
    def _setup_npu_environment():
        """
        Sets up environment for NPU flow of ONNX model and returns saved state to be restored
        later in cleanup.
        """
        oga_path, version = get_oga_npu_dir()

        if not os.path.exists(os.path.join(oga_path, "libs", "onnxruntime.dll")):
            raise RuntimeError(
                f"Cannot find libs/onnxruntime.dll in lib folder: {oga_path}"
            )

        # Save current state so they can be restored after inference.
        saved_state = {"cwd": os.getcwd(), "path": os.environ["PATH"]}

        # Setup NPU environment (cwd and path will be restored later)
        os.chdir(oga_path)
        os.environ["PATH"] = (
            os.path.join(oga_path, "libs") + os.pathsep + os.environ["PATH"]
        )
        if "1.3.0" in version:
            os.environ["DD_ROOT"] = ".\\bins"
            os.environ["DEVICE"] = "stx"
            os.environ["XLNX_ENABLE_CACHE"] = "0"

        return saved_state

    @staticmethod
    def _setup_hybrid_environment():
        """
        Sets up the environment for the Hybrid flow and returns saved state to be restored later
        in cleanup.
        """
        # Determine the Ryzen AI OGA version and hybrid artifacts path
        oga_path, version = get_oga_hybrid_dir()

        if "1.3.0" in version:
            dst_dll = os.path.join(
                oga_path,
                "onnx_utils",
                "bin",
                "DirectML.dll",
            )
            if not os.path.isfile(dst_dll):
                # Artifacts 1.3.0 has DirectML.dll in different subfolder, so copy it to the
                # correct place.  This should not be needed in later RAI release artifacts.
                src_dll = os.path.join(
                    oga_path,
                    "onnxruntime_genai",
                    "lib",
                    "DirectML.dll",
                )
                os.makedirs(os.path.dirname(dst_dll), exist_ok=True)
                shutil.copy2(src_dll, dst_dll)

        saved_state = None
        return saved_state

    @staticmethod
    def _load_model_and_setup_state(
        state, full_model_path, checkpoint, trust_remote_code
    ):
        """
        Loads the OGA model from local folder and then loads the tokenizer.
        Will auto-detect if we're offline.
        """
        state.model = OrtGenaiModel(full_model_path)
        # Auto-detect offline mode
        offline = is_offline()

        try:
            # Always try to use local files first
            local_files_only = True

            hf_tokenizer = AutoTokenizer.from_pretrained(
                full_model_path,
                local_files_only=local_files_only,
                trust_remote_code=trust_remote_code,
            )
        except ValueError as e:
            if "trust_remote_code" in str(e):
                raise ValueError(
                    "This model requires you to execute code from the repo.  Please review it "
                    "and if you trust it, then use the `--trust-remote-code` flag with oga-load."
                )

            if offline and "Can't load tokenizer for" in str(e):
                raise ValueError(
                    f"Cannot load tokenizer for {checkpoint} in offline mode. "
                    f"The tokenizer files may not be available locally in {full_model_path}."
                )
            raise

        state.tokenizer = OrtGenaiTokenizer(
            state.model.model,
            hf_tokenizer,
        )

        status.add_to_state(state=state, name=checkpoint, model=checkpoint)

    @staticmethod
    def _cleanup_environment(saved_state):
        """
        Restores environment to its original state after inference is complete.
        """
        if saved_state:
            os.chdir(saved_state["cwd"])
            os.environ["PATH"] = saved_state["path"]

    def _generate_model_for_hybrid_or_npu(
        self, output_model_path, device, input_model_path
    ):
        """
        Uses a subprocess to run the 'model_generate' command for hybrid or npu devices.
        """

        # Determine the appropriate flag based on the device type
        if device == "hybrid":
            device_flag = "--hybrid"
        elif device == "npu":
            device_flag = "--npu"
        else:
            raise ValueError(f"Unsupported device type for model generation: {device}")

        command = [
            "model_generate",
            device_flag,
            output_model_path,  # Output model directory
            input_model_path,  # Input model directory
        ]

        printing.log_info(f"Running command: {' '.join(command)}")
        try:
            with open(self.logfile_path, "w", encoding="utf-8") as log_file:
                subprocess.run(
                    command, check=True, text=True, stdout=log_file, stderr=log_file
                )
        except FileNotFoundError as e:
            error_message = (
                "The 'model_generate' package is missing from your system. "
                "Ensure all required packages are installed. "
                "To install it, run the following command:\n\n"
                "    lemonade-install --ryzenai <target> --build-model\n"
            )
            raise RuntimeError(error_message) from e

    def run(
        self,
        state: State,
        input: str,
        input_path: str = "",
        device: str = "igpu",
        dtype: str = "int4",
        int4_block_size: int = None,
        force: bool = False,
        download_only: bool = False,
        trust_remote_code=False,
        subfolder: str = None,
    ) -> State:
        # Auto-detect offline status
        offline = is_offline()
        if offline:
            printing.log_warning(
                "Network connectivity to huggingface.co not detected. Running in offline mode."
            )

        state.device = device
        state.dtype = dtype

        # Log initial stats
        state.save_stat(Keys.DTYPE, dtype)
        state.save_stat(Keys.DEVICE, device)
        if device in ["hybrid", "npu"]:
            ryzen_ai_version_info = get_ryzen_ai_version_info()
            state.save_stat(Keys.RYZEN_AI_VERSION_INFO, ryzen_ai_version_info)

        # Check if input is a local folder
        if os.path.isdir(input):
            # input is a local folder
            full_model_path = os.path.abspath(input)
            checkpoint = "local_model"
            state.checkpoint = checkpoint
            state.save_stat(Keys.CHECKPOINT, checkpoint)
            state.save_stat(Keys.LOCAL_MODEL_FOLDER, full_model_path)
            # See if there is a file ending in ".onnx" in this folder
            dir = os.listdir(input)
            has_onnx_file = any([filename.endswith(".onnx") for filename in dir])
            if not has_onnx_file:
                raise ValueError(
                    f"The folder {input} does not contain an ONNX model file."
                )
            if force:
                raise ValueError(
                    "Your input (-i, --input) points to a local folder, which is not "
                    "compatible with the force argument."
                )

        else:
            # input is a model checkpoint
            checkpoint = input
            state.checkpoint = checkpoint
            state.save_stat(Keys.CHECKPOINT, checkpoint)

            # Get base model information
            if not offline:
                base_model = get_base_model(checkpoint)
                if base_model is not None:
                    state.save_stat("base_model", base_model)

            # Setup paths
            full_model_path, model_exists_locally = self._setup_model_paths(
                state, checkpoint, device, dtype, subfolder, int4_block_size
            )

            # If in offline mode, we can only use locally available models
            if offline and not model_exists_locally:
                raise ValueError(
                    f"Model {checkpoint} is not available locally for {device} with {dtype}. "
                    f"Cannot download in offline mode. Check {full_model_path}"
                )

            # Handle download/build if needed
            if (not model_exists_locally) or force:
                if offline:
                    raise ValueError(
                        f"Cannot download or build model {checkpoint} in offline mode"
                    )

                # Validate configuration
                hf_supported = self._validate_model_configuration(
                    device, dtype, checkpoint
                )

                if not hf_supported:
                    raise ValueError(
                        "The (device, dtype, checkpoint) combination is not supported: "
                        f"({device}, {dtype}, {checkpoint})"
                    )
                input_model_path = snapshot_download(
                    repo_id=checkpoint,
                    ignore_patterns=["*.md", "*.txt"],
                    local_files_only=offline,
                )
                # Check if model is ONNX or safetensors
                is_onnx_model = any(
                    [
                        filename.endswith(".onnx")
                        for filename in os.listdir(input_model_path)
                    ]
                )
                is_preoptimized_onnx = is_onnx_model and self._is_preoptimized_model(
                    input_model_path
                )
                is_safetensors_model = any(
                    [
                        filename.endswith(".safetensors")
                        for filename in os.listdir(input_model_path)
                    ]
                )
                if not (is_onnx_model or is_safetensors_model):
                    raise ValueError(
                        f"The model {checkpoint} is not supported. "
                        "It does not contain ONNX or safetensors files."
                    )
                if device in ["npu", "hybrid"]:
                    if is_onnx_model:
                        if is_preoptimized_onnx:
                            # Use HuggingFace cache path as it is
                            full_model_path = input_model_path
                        else:
                            # If ONNX but not modified yet for Hybrid or NPU,
                            # needs further optimization
                            self._generate_model_for_hybrid_or_npu(
                                full_model_path,
                                device,
                                input_model_path,
                            )
                    elif is_safetensors_model:
                        config_path = os.path.join(input_model_path, "config.json")
                        if os.path.exists(config_path):
                            with open(config_path, "r", encoding="utf-8") as f:
                                config = json.load(f)
                            if "quantization_config" in config:
                                # If quantized, use subprocess to generate the model
                                self._generate_model_for_hybrid_or_npu(
                                    full_model_path, device, input_model_path
                                )
                            else:
                                raise ValueError(
                                    f"The safetensors model {checkpoint} is not quantized. "
                                    "Only quantized safetensors models are supported"
                                    " on npu or hybrid targets."
                                )
                        else:
                            raise ValueError(
                                f"config.json not found for safetensors model: {checkpoint}"
                            )
                    else:
                        raise ValueError(
                            f"Unsupported model type for checkpoint: {checkpoint}"
                        )
                else:
                    if is_onnx_model:
                        # Use HuggingFace cache path as it is
                        full_model_path = input_model_path
                    else:
                        self._download_and_build_safetensors_model(
                            checkpoint,
                            device,
                            dtype,
                            full_model_path,
                            int4_block_size,
                            input_path,
                            state,
                        )
                    state.save_stat(Keys.LOCAL_MODEL_FOLDER, full_model_path)

        # Load model if download-only argument is not set
        if not download_only:

            saved_env_state = None
            try:
                if device == "npu":
                    saved_env_state = self._setup_npu_environment()
                    # Set USE_AIE_RoPE based on model type
                    os.environ["USE_AIE_RoPE"] = (
                        "0" if "phi-" in checkpoint.lower() else "1"
                    )
                elif device == "hybrid":
                    saved_env_state = self._setup_hybrid_environment()
                    self._update_hybrid_custom_ops_library_path(full_model_path)

                self._load_model_and_setup_state(
                    state, full_model_path, checkpoint, trust_remote_code
                )
            finally:
                self._cleanup_environment(saved_env_state)

        return state
