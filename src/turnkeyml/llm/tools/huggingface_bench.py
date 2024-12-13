import argparse
import os
from typing import List, Tuple
import time
import statistics
from contextlib import nullcontext
import torch
import tqdm
from turnkeyml.state import State
from turnkeyml.tools import Tool
from turnkeyml.llm.cache import Keys
import turnkeyml.llm.tools.ort_genai.oga_bench as general


def benchmark_huggingface_llm(
    model: torch.nn.Module,
    tokenizer,
    input_ids,
    dtype,
    num_beams: int,
    target_output_tokens: int,
    iterations: int,
    warmup_iterations: int,
) -> List[Tuple[float, int]]:

    # Inform the user whether the current execution is to measure
    # prefill or generation performance, since we need to run this
    # method once for each of those modes
    mode = "prefill" if target_output_tokens == 1 else "generation"

    amp_enabled = True if (dtype == torch.float16 or dtype == torch.bfloat16) else False
    # The "if amp_enabled else nullcontext()" is to get around a bug in PyTorch 2.1
    # where torch.cpu.amp.autocast(enabled=False) does nothing
    with (
        torch.cpu.amp.autocast(enabled=amp_enabled, dtype=dtype)
        if amp_enabled
        else nullcontext()
    ):

        per_iteration_result = []

        # Early stopping is only a valid parameter with multiple beams
        early_stopping = num_beams > 1

        with torch.no_grad(), torch.inference_mode():
            # Don't capture time for warmup
            for _ in tqdm.tqdm(range(warmup_iterations), desc=f"{mode} warmup"):
                model.generate(
                    input_ids,
                    num_beams=num_beams,
                    max_new_tokens=target_output_tokens,
                    min_new_tokens=target_output_tokens,
                    early_stopping=early_stopping,
                    pad_token_id=tokenizer.eos_token_id,
                )

            for _ in tqdm.tqdm(range(iterations), desc=f"{mode} iterations"):
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

                token_len = outputs.shape[1] - input_ids.shape[1]

                # Only count an iteration it produced enough tokens
                if token_len >= target_output_tokens:
                    per_iteration_result.append((latency, token_len))

    return per_iteration_result


class HuggingfaceBench(Tool):
    """
    Benchmarks the performance of the generate() method of an LLM loaded from
    Huggingface Transformers (or any object that supports a
    huggingface-like generate() method).

    Required input state:
        - DTYPE: data type of the model; used to determine if AMP should be
            enabled to convert the input data type to match the model data
            type.
        - MODEL: huggingface-like instance to benchmark.
        - INPUTS: model inputs to pass to generate() during benchmarking.

    Output state produced: None

    """

    unique_name = "huggingface-bench"

    def __init__(self):
        super().__init__(monitor_message="Benchmarking Huggingface LLM")

        self.status_stats = [
            Keys.SECONDS_TO_FIRST_TOKEN,
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND,
        ]

    @staticmethod
    def parser(parser: argparse.ArgumentParser = None, add_help: bool = True):
        # allow inherited classes to initialize and pass in a parser, add parameters to it if so
        if parser is None:
            parser = __class__.helpful_parser(
                short_description="Benchmark a Huggingface-like LLM", add_help=add_help
            )

        parser.add_argument(
            "--iterations",
            "-i",
            required=False,
            type=int,
            default=general.default_iterations,
            help="Number of benchmarking iterations to run (default: "
            f"{general.default_iterations})",
        )

        parser.add_argument(
            "--warmup-iterations",
            "-w",
            required=False,
            type=int,
            default=general.default_warmup_runs,
            help="Number of benchmarking iterations to use for cache warmup "
            "(the results of these iterations "
            f"are not included in the results; default: {general.default_warmup_runs})",
        )

        parser.add_argument(
            "--prompt",
            "-p",
            required=False,
            default=general.default_prompt,
            help="Input prompt to the LLM. Three formats are supported. "
            f"1) integer (default: {general.default_prompt}): "
            "use a synthetic prompt with the specified length. "
            "2) str: use a user-provided prompt string "
            "3) path/to/prompt.txt: load the prompt from a text file.",
        )

        parser.add_argument(
            "--num-beams",
            required=False,
            type=int,
            default=general.default_beams,
            help=f"Number of beams for the LLM to use (default: {general.default_beams})",
        )

        parser.add_argument(
            "--output-tokens",
            required=False,
            type=int,
            default=general.default_output_tokens,
            help="Number of new tokens the LLM should make (default: "
            f"{general.default_output_tokens})",
        )

        return parser

    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:
        """
        Helper function to parse CLI arguments into the args expected
        by run()
        """

        parsed_args = super().parse(state, args, known_only)

        # Decode prompt arg into a string prompt
        if parsed_args.prompt.isdigit():
            # Generate a prompt with the requested length
            length = int(parsed_args.prompt)
            parsed_args.prompt = "word " * (length - 2)

        elif os.path.exists(parsed_args.prompt):
            with open(parsed_args.prompt, "r", encoding="utf-8") as f:
                parsed_args.prompt = f.read()

        else:
            # No change to the prompt
            pass

        return parsed_args

    def run(
        self,
        state: State,
        prompt: str = general.default_prompt,
        iterations: int = general.default_iterations,
        warmup_iterations: int = general.default_warmup_runs,
        num_beams: int = general.default_beams,
        output_tokens: int = general.default_output_tokens,
    ) -> State:
        """
        Args:
            - prompt: input prompt used as a starting point for LLM text generation
            - iterations: number of benchmarking samples to take; results are
                reported as the median and mean of the samples.
            - warmup_iterations: subset of the iterations to treat as warmup,
                and not included in the results.
            - num_beams: number of beams to use in the LLM beam search. If the LLM
                instance has hardcoded its number of beams already, this value
                must match the hardcoded value.
            - output_tokens: Number of new tokens LLM to create.

        We don't have access to the internal timings of generate(), so time to first
        token (TTFT, aka prefill latency) and token/s are calculated using the following formulae:
            prefill_latency = latency of generate(output_tokens=1)
            execution_latency = latency of generate(output_tokens=output_tokens)
            tokens_per_second = (new_tokens - 1) / (execution_latency - prefill_latency)
        """

        if vars(state).get(Keys.MODEL) is None:
            raise ValueError(
                f"{self.__class__.__name__} requires that a model be passed from another tool"
            )

        if vars(state).get("num_beams") and vars(state).get("num_beams") != num_beams:
            raise ValueError(
                f"Number of beams was set to {vars(state).get('num_beams')} "
                f"in a previous tool, but it is set to {num_beams} in "
                "this tool. The values must be the same."
            )

        model = state.model
        tokenizer = state.tokenizer
        dtype = state.dtype

        # Generate the input_ids outside of the benchmarking function to make sure
        # the same input_ids are used everywhere
        input_ids = (
            tokenizer(prompt, return_tensors="pt").to(device=model.device).input_ids
        )

        # Benchmark prefill time (time to first token)
        prefill_per_iteration_result = benchmark_huggingface_llm(
            model=model,
            tokenizer=tokenizer,
            input_ids=input_ids,
            dtype=dtype,
            num_beams=num_beams,
            target_output_tokens=1,
            iterations=iterations,
            warmup_iterations=warmup_iterations,
        )

        time_to_first_token_per_iteration = [
            latency for latency, _ in prefill_per_iteration_result
        ]
        mean_time_to_first_token = statistics.mean(time_to_first_token_per_iteration)

        # Benchmark generation of all tokens
        decode_per_iteration_result = benchmark_huggingface_llm(
            model=model,
            tokenizer=tokenizer,
            input_ids=input_ids,
            dtype=dtype,
            num_beams=num_beams,
            target_output_tokens=output_tokens,
            iterations=iterations,
            warmup_iterations=warmup_iterations,
        )

        mean_execution_latency = statistics.mean(
            [latency for latency, _ in decode_per_iteration_result]
        )
        mean_decode_latency = mean_execution_latency - mean_time_to_first_token
        mean_token_len = statistics.mean(
            [token_len for _, token_len in decode_per_iteration_result]
        )
        # Subtract 1 so that we don't count the prefill token
        token_generation_tokens_per_second = (mean_token_len - 1) / mean_decode_latency

        # Save performance data to stats
        state.save_stat(Keys.SECONDS_TO_FIRST_TOKEN, mean_time_to_first_token)
        state.save_stat(
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND, token_generation_tokens_per_second
        )
        state.save_stat(Keys.PROMPT_TOKENS, input_ids.shape[1])

        return state
