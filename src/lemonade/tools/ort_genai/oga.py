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
from fnmatch import fnmatch
from queue import Queue
from packaging.version import Version
from huggingface_hub import snapshot_download, list_repo_files
import onnxruntime_genai as og
import onnxruntime_genai.models.builder as model_builder
from turnkeyml.state import State
from turnkeyml.tools import FirstTool
import turnkeyml.common.status as status
import turnkeyml.common.printing as printing
from lemonade.tools.huggingface_load import get_base_model
from lemonade.tools.adapter import (
    ModelAdapter,
    TokenizerAdapter,
    PassthroughTokenizerResult,
)
from lemonade.cache import Keys
from lemonade_install.install import (
    DEFAULT_AMD_OGA_NPU_DIR,
    DEFAULT_AMD_OGA_HYBRID_ARTIFACTS_PARENT_DIR,
)


# ONNX Runtime GenAI models will be cached in this subfolder of the lemonade cache folder
oga_models_path = "oga_models"

# ONNX Runtime GenAI model builder tool uses this subfolder of the lemonade cache as its cache
oga_model_builder_cache_path = "model_builder"

# Mapping from processor to executiion provider, used in pathnames and by model_builder
execution_providers = {
    "cpu": "cpu",
    "npu": "npu",
    "igpu": "dml",
    "hybrid": "hybrid",
    "cuda": "cuda",
}


class OrtGenaiTokenizer(TokenizerAdapter):
    def __init__(self, model: og.Model):
        # Initialize the tokenizer and produce the initial tokens.
        self.tokenizer = og.Tokenizer(model)
        # Placeholder value since some code will try to query it
        # If we actually need this to return a proper value, then
        # og.GeneratorParams.eos_token_id has it
        self.eos_token_id = None

    def __call__(self, prompt: str, return_tensors="np"):
        tokens = self.tokenizer.encode(prompt)

        return PassthroughTokenizerResult(tokens)

    # onnxruntime_genai's tokenizer doesn't support any arguments
    # yet, so we just ignore skip_special_tokens and hope it
    # doesn't have a major negative effect
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

        max_length = len(input_ids) + max_new_tokens
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
                max_length=max_length,
                min_length=min_length,
                early_stopping=search_config.get("early_stopping", False),
                length_penalty=search_config.get("length_penalty", 1.0),
                num_beams=search_config.get("num_beams", 1),
                num_return_sequences=search_config.get("num_return_sequences", 1),
                repetition_penalty=search_config.get("repetition_penalty", 1.0),
                past_present_share_buffer=search_config.get(
                    "past_present_share_buffer", True
                ),
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
                max_length=max_length,
                min_length=min_length,
            )
        params.try_graph_capture_with_max_batch_size(1)

        generator = og.Generator(self.model, params)
        if use_oga_post_6_api:
            generator.append_tokens(input_ids)

        if streamer is None:
            prompt_start_time = time.perf_counter()
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

        self.status_stats = [Keys.DTYPE, Keys.DEVICE, Keys.OGA_MODELS_SUBFOLDER]

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
            "--download",
            action="store_true",
            help="Download the model if needed, but don't load it",
        )

        parser.add_argument(
            "--subfolder",
            default=None,
            help="Subfolder where model is located <LEMONADE CACHE>/oga_models/<MODELNAME>"
            "/<SUBFOLDER>, default is <EP for device>-<dtype>.  The EPs are: "
            f'{", ".join([value + " for " + key for key, value in execution_providers.items()])}.',
        )

        return parser

    def run(
        self,
        state: State,
        input: str,
        input_path: str = "",
        device: str = "igpu",
        dtype: str = "int4",
        int4_block_size: int = None,
        force: bool = False,
        download: bool = False,
        subfolder: str = None,
    ) -> State:

        checkpoint = input
        state.checkpoint = checkpoint

        # See whether the device;dtype;checkpoint combination is supported for download from HF
        hf_supported_models = {
            "cpu": {"int4": "*/*", "fp32": "*/*"},
            "igpu": {"int4": "*/*", "fp16": "*/*"},
            "npu": {"int4": "amd/**-onnx-ryzen-strix"},
            "hybrid": {"int4": "amd/**-hybrid"},
            "cuda": {"int4": "*/*", "fp16": "*/*"},
        }

        hf_supported = (
            device in hf_supported_models
            and dtype in hf_supported_models[device]
            and fnmatch(checkpoint, hf_supported_models[device][dtype])
        )

        # Check to see if the model already exists locally
        if subfolder is None:
            subfolder = f"{execution_providers[device]}-{dtype}"
            subfolder += (
                f"-block-{int4_block_size}"
                if dtype == "int4" and int4_block_size is not None
                else ""
            )
        oga_models_subfolder = os.path.join(
            checkpoint.replace("/", "_").lower(), subfolder
        )
        full_model_path = os.path.join(
            state.cache_dir, oga_models_path, oga_models_subfolder
        )
        model_exists_locally = os.path.isdir(full_model_path) and os.listdir(
            full_model_path
        )

        # Check if model needs to be downloaded and/or built or rebuilt
        if not model_exists_locally or force:

            if not hf_supported:
                # Download/build can't be done
                raise ValueError(
                    "The (device, dtype, checkpoint) combination is not supported: "
                    f"({device}, {dtype}, {checkpoint}). The supported combinations "
                    f"for Hugging Face models are "
                    + ", ".join(
                        [
                            f"({dev}, {dt}, {hf_supported_models[dev][dt]})"
                            for dev in hf_supported_models.keys()
                            for dt in hf_supported_models[dev]
                        ]
                    )
                    + "."
                )

            # Check whether the model is a safetensors checkpoint or a pre-exported
            # ONNX model
            # Note: This approach only supports ONNX models where the ONNX files are in the
            #   Huggingface repo root. This does not support the case where the ONNX files
            #   are in a nested directory within the repo.
            model_files = list_repo_files(repo_id=checkpoint)
            onnx_model = any([filename.endswith(".onnx") for filename in model_files])

            # Download the model from HF
            if onnx_model:

                # NPU models on HF are ready to go and HF does its own caching
                full_model_path = snapshot_download(
                    repo_id=checkpoint,
                    ignore_patterns=["*.md", "*.txt"],
                )
                oga_models_subfolder = None

                if device == "hybrid":
                    # Locate the directory containing hybrid-llm-artifacts_1.3.0
                    if os.path.exists(DEFAULT_AMD_OGA_HYBRID_ARTIFACTS_PARENT_DIR):
                        hybrid_artifacts_path = (
                            DEFAULT_AMD_OGA_HYBRID_ARTIFACTS_PARENT_DIR
                        )
                    else:
                        if "AMD_OGA_HYBRID" not in os.environ:
                            raise RuntimeError(
                                "Could not find hybrid-llm-artifacts_1.3.0 in system PATH. "
                                "Please ensure it is added to your PATH environment variable."
                            )

                        hybrid_artifacts_path = os.environ.get("AMD_OGA_HYBRID")

                    if hybrid_artifacts_path:
                        # Construct the path to onnx_custom_ops.dll
                        custom_ops_path = os.path.join(
                            hybrid_artifacts_path,
                            "hybrid-llm-artifacts",
                            "onnx_utils",
                            "bin",
                            "onnx_custom_ops.dll",
                        )

                        config_path = os.path.join(full_model_path, "genai_config.json")

                        # Check if the config file exists
                        if os.path.exists(config_path):
                            with open(config_path, "r", encoding="utf-8") as f:
                                config = json.load(f)

                            # Modify the custom_ops_library under decoder -> session_options
                            if (
                                "model" in config
                                and "decoder" in config["model"]
                                and "session_options" in config["model"]["decoder"]
                            ):
                                config["model"]["decoder"]["session_options"][
                                    "custom_ops_library"
                                ] = custom_ops_path

                            # Write the changes back to the file
                            with open(config_path, "w", encoding="utf-8") as f:
                                json.dump(config, f, indent=4)

                        # Copy DirectML.dll from lib to bin folder
                        src_dll = os.path.join(
                            hybrid_artifacts_path,
                            "hybrid-llm-artifacts",
                            "onnxruntime_genai",
                            "lib",
                            "DirectML.dll",
                        )
                        dst_dll = os.path.join(
                            hybrid_artifacts_path,
                            "hybrid-llm-artifacts",
                            "onnx_utils",
                            "bin",
                            "DirectML.dll",
                        )

                        # Create destination directory if it doesn't exist
                        os.makedirs(os.path.dirname(dst_dll), exist_ok=True)
                        shutil.copy2(src_dll, dst_dll)
            else:
                # checkpoint is safetensors, so we need to run it through model_builder

                # Use model_builder to download model and convert to ONNX
                printing.log_info(f"Building {checkpoint} for {device} using {dtype}")
                extra_options = {}
                if int4_block_size is not None:
                    extra_options["int4-block-size"] = int4_block_size
                try:
                    model_builder.create_model(
                        checkpoint,  # model_name
                        input_path,  # input_path
                        full_model_path,  # output_path
                        dtype,  # precision
                        execution_providers[device],  # execution_provider
                        os.path.join(
                            state.cache_dir, oga_model_builder_cache_path
                        ),  # cache_dir
                        **extra_options,
                    )
                except NotImplementedError as e:
                    # Model architecture is not supported by model builder
                    raise NotImplementedError("[Model builder] " + str(e)) from e
                except OSError as e:
                    # Model is not found either locally nor in HF repository
                    raise ValueError("[Model builder] " + str(e)) from e

        if not download:
            # The download only flag is not set, so load model
            if device == "npu":
                if os.path.exists(DEFAULT_AMD_OGA_NPU_DIR):
                    oga_path = os.path.join(DEFAULT_AMD_OGA_NPU_DIR, "amd_oga")
                else:
                    if "AMD_OGA" not in os.environ:
                        raise RuntimeError(
                            "Please set environment variable AMD_OGA "
                            "to the path of the amd_oga files"
                        )

                    # Check AMD_OGA points to oga library files
                    oga_path = os.environ["AMD_OGA"]
                if not os.path.exists(
                    os.path.join(oga_path, "libs", "onnxruntime.dll")
                ):
                    raise RuntimeError(
                        f"Cannot find libs/onnxruntime.dll in AMD_OGA folder: {oga_path}"
                    )

                # Save current directory and PATH
                saved_cwd = os.getcwd()
                saved_path = os.environ["PATH"]

                # Change to the AMD_OGA distribution directory
                os.chdir(oga_path)
                os.environ["PATH"] += os.pathsep + os.path.join(oga_path, "libs")

                # Common environment variables for all NPU models
                os.environ["DD_ROOT"] = ".\\bins"
                os.environ["DEVICE"] = "stx"
                os.environ["XLNX_ENABLE_CACHE"] = "0"

                # Phi models require USE_AIE_RoPE=0
                if "phi-" in checkpoint.lower():
                    os.environ["USE_AIE_RoPE"] = "0"
                else:
                    os.environ["USE_AIE_RoPE"] = "1"

            state.model = OrtGenaiModel(full_model_path)
            state.tokenizer = OrtGenaiTokenizer(state.model.model)
            state.dtype = dtype

            state.save_stat(Keys.CHECKPOINT, checkpoint)
            state.save_stat(Keys.DTYPE, dtype)
            state.save_stat(Keys.DEVICE, device)
            if oga_models_subfolder is not None:
                state.save_stat(Keys.OGA_MODELS_SUBFOLDER, oga_models_subfolder)

            # Get base model information
            base_model = get_base_model(checkpoint)
            if base_model is not None:
                state.save_stat("base_model", base_model)

            # Create a UniqueInvocationInfo and ModelInfo so that we can display status
            # at the end of the sequence
            status.add_to_state(state=state, name=input, model=input)

            if device == "npu":
                # Restore cwd and PATH
                os.chdir(saved_cwd)
                os.environ["PATH"] = saved_path

        return state
