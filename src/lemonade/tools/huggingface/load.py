import argparse
from typing import Dict, Optional
import json
from lemonade.tools import FirstTool
from lemonade.state import State
import lemonade.common.status as status
import lemonade.common.printing as printing
from lemonade.cache import Keys


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

    def _imports(self):
        pass

    def __init__(self):
        super().__init__(monitor_message="Loading Huggingface checkpoint")

        self.status_stats = [Keys.DTYPE]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load an LLM in PyTorch using huggingface transformers",
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

        from lemonade.tools.huggingface.utils import str_to_dtype

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
        dtype: "torch.dtype" = None,
        device: Optional[str] = None,
        load_kwargs: Optional[Dict] = None,
        channels_last: bool = True,
    ) -> State:
        # Import expensive modules at runtime
        import transformers
        import torch

        from lemonade.tools.huggingface.utils import (
            HuggingfaceTokenizerAdapter,
            HuggingfaceAdapter,
        )
        from lemonade.common.network import (
            is_offline,
            get_base_model,
        )

        # Set default dtype
        if dtype is None:
            dtype_to_use = torch.float32
        else:
            dtype_to_use = dtype

        # Auto-detect offline status
        offline = is_offline()
        if offline:
            printing.log_warning(
                "Network connectivity to huggingface.co not detected. Running in offline mode."
            )

        checkpoint = input

        if load_kwargs is None:
            load_kwargs_to_use = {}
        else:
            load_kwargs_to_use = load_kwargs

        # Add local_files_only to kwargs in offline mode
        if offline:
            load_kwargs_to_use["local_files_only"] = True

        if vars(state).get(Keys.MODEL):
            raise ValueError("HuggingfaceLoad must be the first tool in the sequence")

        try:
            model = transformers.AutoModelForCausalLM.from_pretrained(
                checkpoint,
                torch_dtype=dtype_to_use,
                low_cpu_mem_usage=True,
                **load_kwargs_to_use,
            )
        except Exception as e:
            if offline and "Can't load config for" in str(e):
                raise ValueError(
                    f"Cannot load model {checkpoint} in offline mode. "
                    f"The model files may not be available locally. Original error: {str(e)}"
                )
            raise

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
            tokenizer_kwargs = {
                "use_fast": False,
                "model_max_length": 4096,
                "padding_side": "left",
            }
            if offline:
                tokenizer_kwargs["local_files_only"] = True

            tokenizer = transformers.AutoTokenizer.from_pretrained(
                checkpoint, **tokenizer_kwargs
            )
        except ValueError as e:
            # Sometimes those specific tokenizer flags are not supported, in which
            # case we try to just load a simple tokenizer
            tokenizer_kwargs = {}
            if offline:
                tokenizer_kwargs["local_files_only"] = True

            try:
                tokenizer = transformers.AutoTokenizer.from_pretrained(
                    checkpoint, **tokenizer_kwargs
                )
            except Exception as e:
                if offline and "Can't load tokenizer for" in str(e):
                    raise ValueError(
                        f"Cannot load tokenizer for {checkpoint} in offline mode. "
                        f"The tokenizer files may not be available locally. "
                        f"Original error: {str(e)}"
                    )
                raise

        # Pass the model and inputs into state
        state.model = HuggingfaceAdapter(model, dtype_to_use, device, tokenizer)

        state.tokenizer = HuggingfaceTokenizerAdapter(tokenizer, device)
        state.dtype = dtype_to_use
        state.checkpoint = checkpoint
        state.device = device

        # Save stats about the model
        state.save_stat(Keys.CHECKPOINT, checkpoint)
        state.save_stat(Keys.DTYPE, str(dtype_to_use).split(".")[1])
        state.save_stat(Keys.DEVICE, device)

        # Get base model information
        base_model = get_base_model(checkpoint)
        if base_model is not None:
            state.save_stat("base_model", base_model)

        # Create a UniqueInvocationInfo and ModelInfo so that we can display status
        # at the end of the sequence
        status.add_to_state(state=state, name=input, model=model)

        return state


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
