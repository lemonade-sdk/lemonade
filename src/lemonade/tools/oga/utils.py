import os
import time
import json
import logging
from queue import Queue
from packaging.version import Version
import onnxruntime_genai as og
from transformers import AutoTokenizer
from lemonade.tools.adapter import (
    ModelAdapter,
    TokenizerAdapter,
    PassthroughTokenizerResult,
)
from lemonade_install.install import _get_ryzenai_version_info


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
        rai_config_path = os.path.join(input_folder, "rai_config.json")
        max_prompt_length = None

        try:
            detected_version, _ = _get_ryzenai_version_info()

            if os.path.exists(rai_config_path):
                with open(rai_config_path, "r", encoding="utf-8") as f:
                    rai_config = json.load(f)
                    if (
                        "max_prompt_length" in rai_config
                        and detected_version in rai_config["max_prompt_length"]
                    ):
                        max_prompt_length = rai_config["max_prompt_length"][
                            detected_version
                        ]
        except:  # pylint: disable=bare-except
            pass

        config_path = os.path.join(input_folder, "genai_config.json")
        if os.path.exists(config_path):
            with open(config_path, "r", encoding="utf-8") as f:
                config_dict = json.load(f)
                config_dict["max_prompt_length"] = max_prompt_length
                return config_dict
        return None

    def generate(
        self,
        input_ids,
        max_new_tokens=512,
        min_new_tokens=0,
        do_sample=True,
        top_k=None,
        top_p=None,
        temperature=None,
        repeat_penalty=None,
        streamer: OrtGenaiStreamer = None,
        pad_token_id=None,
        stopping_criteria=None,
        max_length=None,
        random_seed=1,
    ):
        params = og.GeneratorParams(self.model)

        # OGA models return a list of tokens (older versions) or 1d numpy array (newer versions)
        prompt_length = len(input_ids)

        max_prompt_length = self.config.get("max_prompt_length")
        if max_prompt_length and prompt_length > max_prompt_length:
            raise ValueError(
                f"This prompt (length {prompt_length}) exceeds the model's "
                f"maximum allowed prompt length ({max_prompt_length})."
            )
        self.prompt_tokens = prompt_length

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
            max_length_to_use = prompt_length + max_new_tokens

        min_length = prompt_length + min_new_tokens

        if use_oga_pre_6_api:
            params.input_ids = input_ids

        if random_seed is None:
            random_seed = -1  # In og.Generator, -1 = seed with random device

        # Get search config if available, otherwise use empty dict
        # Thanks to the empty dict, if the model doesn't have a built-in search
        #   config, the .get() calls will all just use the default values
        search_config = {}
        if self.config and "search" in self.config:
            search_config = self.config["search"]

        # Apply parameter hierarchy: user provided > search config > defaults
        default_top_k = 50
        default_top_p = 1.0
        default_temperature = 0.7
        default_repetition_penalty = 1.0

        top_k_to_use = (
            top_k if top_k is not None else search_config.get("top_k", default_top_k)
        )
        top_p_to_use = (
            top_p if top_p is not None else search_config.get("top_p", default_top_p)
        )
        temperature_to_use = (
            temperature
            if temperature is not None
            else search_config.get("temperature", default_temperature)
        )
        # Map the llamacpp name, `repeat_penalty`, to the OGA name, `repetition_penalty`
        repetition_penalty_to_use = (
            repeat_penalty
            if repeat_penalty is not None
            else search_config.get("repetition_penalty", default_repetition_penalty)
        )

        # Set search options once with all parameters
        params.set_search_options(
            do_sample=search_config.get("do_sample", do_sample),
            top_k=top_k_to_use,
            top_p=top_p_to_use,
            temperature=temperature_to_use,
            repetition_penalty=repetition_penalty_to_use,
            max_length=max_length_to_use,
            min_length=min_length,
            early_stopping=search_config.get("early_stopping", False),
            length_penalty=search_config.get("length_penalty", 1.0),
            num_beams=search_config.get("num_beams", 1),
            num_return_sequences=search_config.get("num_return_sequences", 1),
            past_present_share_buffer=search_config.get(
                "past_present_share_buffer", True
            ),
            random_seed=random_seed,
            # Not currently supported by OGA
            # diversity_penalty=search_config.get('diversity_penalty', 0.0),
            # no_repeat_ngram_size=search_config.get('no_repeat_ngram_size', 0),
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

            response = generator.get_sequence(0)
            self.response_tokens = len(response) - self.prompt_tokens
            return [response]
        else:
            if use_oga_post_6_api:
                generator.append_tokens(input_ids)
            tokenizer_stream = streamer.tokenizer.tokenizer.create_stream()
            self.response_tokens = 0
            stop_early = False

            while not generator.is_done() and not stop_early:
                if use_oga_pre_6_api:
                    generator.compute_logits()
                generator.generate_next_token()
                self.response_tokens += 1

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
