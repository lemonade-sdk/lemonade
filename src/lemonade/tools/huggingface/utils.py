from typing import Dict, List, Tuple
import time
from contextlib import nullcontext
import transformers
import torch
from lemonade.state import State
from lemonade.tools.adapter import TokenizerAdapter
from lemonade.tools.adapter import ModelAdapter
from lemonade.tools.bench import Bench

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
        random_seed=1,
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

        if random_seed is None:
            torch.random.seed()
        else:
            torch.random.manual_seed(random_seed)

        with torch.no_grad(), torch.inference_mode():
            outputs = self.model.generate(input_ids=input_ids, **generation_kwargs)

        self.prompt_tokens = input_ids.shape[1]
        self.response_tokens = len(outputs[0]) - self.prompt_tokens
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


def benchmark_huggingface_llm(
    model: torch.nn.Module,
    tokenizer,
    input_ids,
    dtype,
    num_beams: int,
    target_output_tokens: int,
    iterations: int,
    warmup_iterations: int,
    report_progress_fn,
) -> List[Tuple[float, int]]:

    amp_enabled = True if (dtype == torch.float16 or dtype == torch.bfloat16) else False
    # The "if amp_enabled else nullcontext()" is to get around a bug in PyTorch 2.1
    # where torch.cpu.amp.autocast(enabled=False) does nothing
    with (
        torch.cpu.amp.autocast(enabled=amp_enabled, dtype=dtype)
        if amp_enabled
        else nullcontext()
    ):

        per_iteration_result = []
        tokens_out_len_list = []

        # Early stopping is only a valid parameter with multiple beams
        early_stopping = num_beams > 1

        with torch.no_grad(), torch.inference_mode():
            # Don't capture time for warmup
            for count in range(warmup_iterations):
                outputs = model.generate(
                    input_ids,
                    num_beams=num_beams,
                    max_new_tokens=target_output_tokens,
                    min_new_tokens=target_output_tokens,
                    early_stopping=early_stopping,
                    pad_token_id=tokenizer.eos_token_id,
                )
                tokens_out_len_list.append(outputs.shape[1] - input_ids.shape[1])
                report_progress_fn((count + 1) / (warmup_iterations + iterations))

            for count in range(iterations):
                # CUDA synchronization is required prior to GPU benchmarking
                # This has no negative effect on CPU-only benchmarks, and is more robust than
                # checking `model.device == "cuda"` since it applies to multi-GPU environments
                # Synchronization is done before collecting the start time because this will
                # ensure that the GPU has finished initialization tasks such as loading weights
                if torch.cuda.is_available():
                    torch.cuda.synchronize()
                start_time = time.perf_counter()

                outputs = model.generate(
                    input_ids,
                    num_beams=num_beams,
                    max_new_tokens=target_output_tokens,
                    min_new_tokens=target_output_tokens,
                    early_stopping=early_stopping,
                    pad_token_id=tokenizer.eos_token_id,
                )

                if torch.cuda.is_available():
                    torch.cuda.synchronize()
                end_time = time.perf_counter()

                latency = end_time - start_time

                tokens_out_len_list.append(model.response_tokens)

                # Only count an iteration if it produced enough tokens
                if model.response_tokens >= target_output_tokens:
                    per_iteration_result.append((latency, model.response_tokens))

                report_progress_fn(
                    (warmup_iterations + count + 1) / (warmup_iterations + iterations)
                )

        if not per_iteration_result:
            raise Bench.not_enough_tokens(target_output_tokens)

    return per_iteration_result, tokens_out_len_list
