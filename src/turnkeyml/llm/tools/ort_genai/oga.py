# onnxruntime_genai is not lint-friendly yet and PyLint can't
# find any of the class methods
# pylint: disable=no-member

import argparse
import os
import time
import json
from fnmatch import fnmatch
from queue import Queue
from huggingface_hub import snapshot_download, login
import onnxruntime_genai as og
from turnkeyml.state import State
from turnkeyml.tools import FirstTool
import turnkeyml.common.status as status
from turnkeyml.llm.tools.adapter import (
    ModelAdapter,
    TokenizerAdapter,
    PassthroughTokenizerResult,
)
from turnkeyml.llm.cache import Keys


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
        do_sample=True,
        top_k=50,
        top_p=1.0,
        temperature=0.7,
        streamer: OrtGenaiStreamer = None,
        pad_token_id=None,
        stopping_criteria=None,
    ):
        params = og.GeneratorParams(self.model)

        if pad_token_id:
            params.pad_token_id = pad_token_id

        max_length = len(input_ids) + max_new_tokens

        params.input_ids = input_ids
        if self.config and "search" in self.config:
            search_config = self.config["search"]
            params.set_search_options(
                do_sample=search_config.get("do_sample", do_sample),
                top_k=search_config.get("top_k", top_k),
                top_p=search_config.get("top_p", top_p),
                temperature=search_config.get("temperature", temperature),
                max_length=max_length,
                min_length=0,
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
                min_length=max_length,
            )
        params.try_graph_capture_with_max_batch_size(1)

        generator = og.Generator(self.model, params)

        if streamer is None:
            prompt_start_time = time.perf_counter()
            generator.compute_logits()
            generator.generate_next_token()
            prompt_end_time = time.perf_counter()

            self.time_to_first_token = prompt_end_time - prompt_start_time

            if max_new_tokens > 1:

                token_gen_times = []
                while not generator.is_done():
                    token_gen_start_time = time.perf_counter()
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
                generator.compute_logits()
                generator.generate_next_token()

                new_token = generator.get_next_tokens()[0]
                new_text = tokenizer_stream.decode(new_token)

                streamer.add_text(new_text)

                if stopping_criteria is not None:
                    if stopping_criteria[0].stop_event.is_set():
                        stop_early = True

            streamer.add_text("</s>")
            streamer.done()


# Short names for checkpoints
# So that we don't violate pylint line lengths :)
llama_3 = "meta-llama/Meta-Llama-3-8B"
llama_2 = "meta-llama/Llama-2-7b-chat-hf"
phi_3_mini_4k = "microsoft/Phi-3-mini-4k-instruct"
phi_3_mini_128k = "microsoft/Phi-3-mini-128k-instruct"
qwen_1dot5 = "Qwen/Qwen1.5-7B"


class OgaLoad(FirstTool):
    """
    Tool that loads an LLM in OnnxRuntime-GenAI for use with DirectML.

    Input: path to a checkpoint. Supported choices:
        llama_3 = "meta-llama/Meta-Llama-3-8B"
        llama_2 = "meta-llama/Llama-2-7b-chat-hf"
        phi_3_mini_4k = "microsoft/Phi-3-mini-4k-instruct"
        phi_3_mini_128k = "microsoft/Phi-3-mini-128k-instruct"
        And models on Hugging Face that follow the "amd/**-onnx-ryzen-strix" pattern

    Output:
        state.model: handle to a Huggingface-style LLM loaded on DirectML device
        state.tokenizer = Huggingface-style LLM tokenizer instance
        state.dtype = data type of the model on DirectML device

    Note: This tool expects the onnxruntime-genai-directml library to be pre-installed.
            If that library is not installed, this tool will not load.
    """

    unique_name = "oga-load"

    def __init__(self):
        super().__init__(monitor_message="Loading OnnxRuntime-GenAI model")

        self.status_stats = [Keys.DTYPE, Keys.DEVICE]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load model in onnxruntime-genai (OGA)",
            add_help=add_help,
        )

        parser.add_argument(
            "-d",
            "--device",
            choices=["igpu", "npu", "cpu"],
            default="igpu",
            help="Which device to load the model on to (default: igpu)",
        )

        parser.add_argument(
            "--dtype",
            choices=["int4"],
            required=True,
            help="Data type to load the model in",
        )

        return parser

    def run(
        self,
        state: State,
        input: str = phi_3_mini_128k,
        device: str = "igpu",
        dtype: str = "int4",
    ) -> State:

        checkpoint = input

        # Map of models[device][dtype][checkpoint] to the name of the model folder on disk
        local_supported_models = {
            "igpu": {
                "int4": {
                    phi_3_mini_128k: os.path.join(
                        "phi-3-mini-128k-instruct",
                        "directml",
                        "directml-int4-awq-block-128",
                    ),
                    phi_3_mini_4k: os.path.join(
                        "phi-3-mini-4k-instruct",
                        "directml",
                        "directml-int4-awq-block-128",
                    ),
                },
            },
            "npu": {
                "int4": {
                    # Legacy RyzenAI 1.2 models for NPU
                    llama_2: "llama2-7b-int4",
                    llama_3: "llama3-8b-int4",
                    qwen_1dot5: "qwen1.5-7b-int4",
                }
            },
            "cpu": {
                "int4": {
                    phi_3_mini_4k: os.path.join(
                        "phi-3-mini-4k-instruct",
                        "cpu_and_mobile",
                        "cpu-int4-rtn-block-32-acc-level-4",
                    ),
                }
            },
        }

        hf_supported_models = {"npu": {"int4": "amd/**-onnx-ryzen-strix"}}

        supported_locally = True
        try:
            dir_name = local_supported_models[device][dtype][checkpoint]
        except KeyError as e:
            supported_locally = False
            hf_supported = (
                device in hf_supported_models
                and dtype in hf_supported_models[device]
                and fnmatch(checkpoint, hf_supported_models[device][dtype])
            )
            if not hf_supported:
                raise ValueError(
                    "The device;dtype;checkpoint combination is not supported: "
                    f"{device};{dtype};{checkpoint}. The supported combinations "
                    f"are: {local_supported_models} for local models and {hf_supported_models}"
                    " for models on Hugging Face."
                ) from e

        # Create models dir if it doesn't exist
        models_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "models")
        if not os.path.exists(models_dir):
            os.makedirs(models_dir)

        # If the model is supported though Hugging Face, download it
        if not supported_locally:
            hf_model_name = checkpoint.split("amd/")[1]
            dir_name = "_".join(hf_model_name.split("-")[:6]).lower()
            api_key = os.getenv("HF_TOKEN")
            login(api_key)
            snapshot_download(
                repo_id=checkpoint,
                local_dir=os.path.join(models_dir, dir_name),
                ignore_patterns=["*.md", "*.txt"],
            )

        current_cwd = os.getcwd()
        if device == "npu":
            # Change to the models directory
            os.chdir(models_dir)

            # Common environment variables for all NPU models
            os.environ["DD_ROOT"] = ".\\bins"
            os.environ["DEVICE"] = "stx"
            os.environ["XLNX_ENABLE_CACHE"] = "0"

            # Phi models require USE_AIE_RoPE=0
            if "phi-" in checkpoint.lower():
                os.environ["USE_AIE_RoPE"] = "0"
            else:
                os.environ["USE_AIE_RoPE"] = "1"

        model_dir = os.path.join(models_dir, dir_name)
        state.model = OrtGenaiModel(model_dir)
        state.tokenizer = OrtGenaiTokenizer(state.model.model)
        state.dtype = dtype

        state.save_stat(Keys.CHECKPOINT, checkpoint)
        state.save_stat(Keys.DTYPE, dtype)
        state.save_stat(Keys.DEVICE, device)

        # Create a UniqueInvocationInfo and ModelInfo so that we can display status
        # at the end of the sequence
        status.add_to_state(state=state, name=input, model=input)

        # Put the CWD back to its original value
        os.chdir(current_cwd)

        return state
