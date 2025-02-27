import argparse
from typing import Dict, Optional
import json
import transformers
import torch
from huggingface_hub import model_info
from turnkeyml.state import State
import turnkeyml.common.status as status
from turnkeyml.tools import Tool, FirstTool
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter
from lemonade.cache import Keys

# Command line interfaces for tools will use string inputs for data
# types, however the internal tool logic will need to know the actual
# torch type
str_to_dtype = {
    "float32": torch.float32,
    "float16": torch.float16,
    "bfloat16": torch.bfloat16,
    "int8_static": torch.int8,
    "int8_dynamic": torch.int8,
}


def make_example_inputs(state: State) -> Dict:
    """
    Create a dictionary of LLM inputs that can be passed as an argument
    into quantization, ONNX export, etc.
    """

    tokenizer = state.tokenizer
    inputs_ids = tokenizer("Hello there", return_tensors="pt").input_ids
    return {"input_ids": inputs_ids}


class HuggingfaceTokenizerAdapter(TokenizerAdapter):
    def __init__(self, tokenizer: transformers.AutoTokenizer, device: str):
        super().__init__()
        self.tokenizer = tokenizer
        self.device = device

    def __call__(self, prompt, **kwargs):
        tokens = self.tokenizer(prompt, **kwargs)
        if self.device:
            return tokens.to(self.device)
        else:
            return tokens

    def decode(self, response, **kwargs):
        return self.tokenizer.decode(response, **kwargs)

    def batch_decode(self, tokens, **kwargs):
        return self.tokenizer.batch_decode(tokens, **kwargs)

    @property
    def eos_token_id(self):
        return self.tokenizer.eos_token_id

    def save_pretrained(self, model_dir, **kwargs):
        return self.tokenizer.save_pretrained(model_dir, **kwargs)


def get_base_model(checkpoint: str) -> Optional[str]:
    """
    Get the base model information for a given checkpoint from the Hugging Face Hub.

    Args:
        checkpoint: The model checkpoint to query

    Returns:
        The base model name if found, or None if not found or error occurs
    """
    try:
        info = model_info(checkpoint)
        if info.cardData and "base_model" in info.cardData:
            if info.cardData["base_model"] is not None:
                # This is a derived model
                return info.cardData["base_model"]
            else:
                # This is itself a base model
                return checkpoint
    except Exception:  # pylint: disable=broad-except
        pass
    return None


class HuggingfaceLoad(FirstTool):
    """
    Load an LLM as a torch.nn.Module using the Hugging Face transformers
    from_pretrained() API.

    Expected input: a checkpoint to load

    Output state produced:
        - state.model: instance of torch.nn.Module that implements an LLM.
        - state.inputs: tokenized example inputs to the model, in the form of a
            dictionary of kwargs.
        - state.tokenizer: instance of Hugging Face PretrainedTokenizer.
        - state.dtype: data type of the model.
        - state.checkpoint: pretrained checkpoint used to load the model.
    """

    unique_name = "huggingface-load"

    def __init__(self):
        super().__init__(monitor_message="Loading Huggingface checkpoint")

        self.status_stats = [Keys.DTYPE]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load an LLM as torch.nn.Module using huggingface from_pretrained()",
            add_help=add_help,
        )

        default_dtype = "float32"
        parser.add_argument(
            "--dtype",
            "-d",
            required=False,
            default=default_dtype,
            help=f"Data type to load the model in (default: {default_dtype}).",
        )

        choices = ["cpu", "cuda"]
        for cuda in range(15):
            choices.append(f"cuda:{cuda}")
        parser.add_argument(
            "--device",
            required=False,
            default=None,
            choices=choices,
            help="Move the model and inputs to a device using the .to() method "
            "(default: don't call the .to() method)",
        )

        parser.add_argument(
            "--load-kwargs",
            required=False,
            default="{}",
            type=json.loads,
            help="Arbitrary kwargs, in json format, that will be passed as "
            "from_pretrained(**kwargs). "
            r"Example: --load-kwargs='{\"trust_remote_code\": true} would result in "
            "from_pretrained(trust_remote_code=True)",
        )

        parser.add_argument(
            "--channels-last",
            default=True,
            type=bool,
            help="Whether to format the model in memory using "
            "channels-last (default: True)",
        )

        return parser

    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:

        parsed_args = super().parse(state, args, known_only)

        # Save stats about the user's input (do this prior to decoding)
        state.save_stat(Keys.CHECKPOINT, parsed_args.input)
        state.save_stat(Keys.DTYPE, parsed_args.dtype)

        # Decode dtype arg into a torch value
        parsed_args.dtype = str_to_dtype[parsed_args.dtype]

        return parsed_args

    def run(
        self,
        state: State,
        input: str = "",
        dtype: torch.dtype = torch.float32,
        device: Optional[str] = None,
        load_kwargs: Optional[Dict] = None,
        channels_last: bool = True,
    ) -> State:

        checkpoint = input

        if load_kwargs is None:
            load_kwargs_to_use = {}
        else:
            load_kwargs_to_use = load_kwargs

        if vars(state).get(Keys.MODEL):
            raise ValueError("HuggingfaceLoad must be the first tool in the sequence")

        model = transformers.AutoModelForCausalLM.from_pretrained(
            checkpoint,
            torch_dtype=dtype,
            low_cpu_mem_usage=True,
            **load_kwargs_to_use,
        )

        # Only call the model.to() method if an argument to this function
        # provides a reason to do so
        to_args = {}
        if channels_last:
            to_args["memory_format"] = torch.channels_last
        if device:
            to_args["device"] = device
        if to_args:
            model.to(**to_args)

        model = model.eval()

        try:
            tokenizer = transformers.AutoTokenizer.from_pretrained(
                checkpoint, use_fast=False, model_max_length=4096, padding_side="left"
            )
        except ValueError:
            # Sometimes those specific tokenizer flags are not supported, in which
            # case we try to just load a simple tokenizer
            tokenizer = transformers.AutoTokenizer.from_pretrained(checkpoint)

        # Pass the model and inputs into state
        state.model = model
        state.tokenizer = HuggingfaceTokenizerAdapter(tokenizer, device)
        state.dtype = dtype
        state.checkpoint = checkpoint
        state.device = device

        # Save stats about the model
        state.save_stat(Keys.CHECKPOINT, checkpoint)
        state.save_stat(Keys.DTYPE, str(dtype).split(".")[1])
        state.save_stat(Keys.DEVICE, device)

        # Get base model information
        base_model = get_base_model(checkpoint)
        if base_model is not None:
            state.save_stat("base_model", base_model)

        # Create a UniqueInvocationInfo and ModelInfo so that we can display status
        # at the end of the sequence
        status.add_to_state(state=state, name=input, model=model)

        return state


class HuggingfaceAdapter(ModelAdapter):
    """
    Wrapper class for Huggingface LLMs that set generate() arguments to
    make them more accurate and pleasant to chat with:

        repetition_penalty: helps the LLM avoid repeating the same short
            phrase in the response over and over.
        temperature: helps the LLM stay focused on the prompt.
        do_sample: apply the temperature.
    """

    def __init__(self, model, dtype=torch.float32, device="cpu"):
        super().__init__()
        self.model = model
        self.dtype = dtype
        self.device = device

    def generate(
        self,
        input_ids,
        max_new_tokens=512,
        repetition_penalty=1.2,
        do_sample=True,
        temperature=0.1,
        **kwargs,
    ):
        amp_enabled = (
            True
            if (self.dtype == torch.float16 or self.dtype == torch.bfloat16)
            else False
        )

        # Move input_ids to the same device as the model
        input_ids = input_ids.to(self.device)

        with torch.no_grad(), torch.inference_mode(), torch.cpu.amp.autocast(
            enabled=amp_enabled, dtype=self.dtype
        ):
            return self.model.generate(
                input_ids=input_ids,
                max_new_tokens=max_new_tokens,
                repetition_penalty=repetition_penalty,
                do_sample=do_sample,
                temperature=temperature,
                **kwargs,
            )


class AdaptHuggingface(Tool):
    """
    Apply specific settings to make Huggingface LLMs
    more accurate and pleasant to chat with.
    """

    unique_name = "adapt-huggingface"

    def __init__(self):
        super().__init__(monitor_message="Adapting Huggingface LLM")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Apply accuracy-boosting settings to huggingface LLMs",
            add_help=add_help,
        )

        return parser

    def run(self, state: State) -> State:

        state.model = HuggingfaceAdapter(state.model, state.dtype, state.device)

        return state
