import argparse
from typing import Dict, Optional
import json
import socket
import transformers
import torch
from huggingface_hub import model_info
from turnkeyml.state import State
import turnkeyml.common.status as status
import turnkeyml.common.printing as printing
from turnkeyml.tools import FirstTool
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
        super().__init__(tokenizer)
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


def is_offline():
    """
    Check if the system is offline by attempting to connect to huggingface.co.

    Returns:
        bool: True if the system is offline (cannot connect to huggingface.co),
              False otherwise.
    """
    try:
        socket.gethostbyname("huggingface.co")
        return False
    except socket.gaierror:
        return True


def get_base_model(checkpoint: str) -> Optional[str]:
    """
    Get the base model information for a given checkpoint from the Hugging Face Hub.
    Will auto-detect if we're offline and skip the network call in that case.

    Args:
        checkpoint: The model checkpoint to query

    Returns:
        The base model name if found, or None if not found or error occurs
    """
    # Skip network call in offline mode
    if is_offline():
        return None

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
                torch_dtype=dtype,
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
        state.model = HuggingfaceAdapter(model, dtype, device, tokenizer)

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
    Wrapper class for Huggingface LLMs that handle generation arguments
    from callers to match HF specification.

        repetition_penalty: helps the LLM avoid repeating the same short
            phrase in the response over and over.
        temperature: helps the LLM stay focused on the prompt.
        do_sample: apply the temperature.
    """

    def __init__(self, model, dtype=torch.float32, device="cpu", tokenizer=None):
        super().__init__()
        self.model = model
        self.dtype = dtype
        self.device = device
        self.tokenizer = tokenizer

    def generate(
        self,
        input_ids,
        **kwargs,
    ):

        # Move input_ids to the same device as the model
        input_ids = input_ids.to(self.device)

        # Fix temperature handling to avoid errors:
        # If temperature is 0.0, force do_sample=False (greedy decoding)
        if kwargs.get("temperature") == 0.0:
            kwargs["do_sample"] = False

        # If do_sample is False and temperature is 0.0, remove temperature
        # to avoid the warning from HuggingFace.
        # Note: This is the same approach taken by LM Eval Harness for handling temperature.
        generation_kwargs = {
            "max_new_tokens": kwargs.get("max_new_tokens", 512),
            "do_sample": kwargs.get("do_sample", True),
            **kwargs,
        }

        with torch.no_grad(), torch.inference_mode():
            outputs = self.model.generate(input_ids=input_ids, **generation_kwargs)

            return outputs

    def _model_call(self, input_tensor):
        """Forward pass through the model to get logits

        This method directly calls the model forward pass rather than using model.generate() for
        several important reasons:
        1. Purpose: We need raw logits from a single forward pass, while generate() is for producing
           multiple tokens through iterative inference
        2. Efficiency: Direct calls are more efficient for logprob calculations with no sampling
           overhead
        3. Precision: Logprob calculations require exact control over input-to-output mapping
        4. Consistency: Similar approach used in both HF and OGA implementations

        Args:
            input_tensor: Input token IDs tensor

        Returns:
            Logits tensor from model forward pass
        """
        with torch.no_grad(), torch.inference_mode():
            outputs = self.model(input_tensor)
            return outputs.logits

    def _select_cont_toks(self, logits, context_len, cont_toks):
        """
        Select logits corresponding to continuation tokens and gather their probabilities

        Args:
            logits: Model output logits
            context_len: Length of input context
            cont_toks: List of continuation token IDs

        Returns:
            Tensor of log probabilities for continuation tokens
        """
        # Get the continuation logits (discard context logits)
        cont_logits = logits[context_len - 1 : context_len - 1 + len(cont_toks)]

        # Convert cont_toks to tensor if needed
        if not isinstance(cont_toks, torch.Tensor):
            cont_toks = torch.tensor(cont_toks, dtype=torch.long, device=logits.device)

        # Gather log probs at the corresponding token indices
        log_probs = torch.log_softmax(cont_logits, dim=-1)
        token_log_probs = torch.gather(log_probs, 1, cont_toks.unsqueeze(-1)).squeeze(
            -1
        )

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
        if tokenizer is None:
            raise ValueError("Tokenizer is required for logprob calculation")

        # Encode the full text
        tokens = tokenizer(text).input_ids

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

        # Convert to tensor and get model output
        input_tensor = torch.tensor([tokens], dtype=torch.long, device=self.device)
        logits = self._model_call(input_tensor)[0]

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
