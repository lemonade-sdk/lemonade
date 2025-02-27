import argparse
from typing import List, Tuple
import time
import statistics
from statistics import StatisticsError
from contextlib import nullcontext
import torch
import tqdm
from turnkeyml.state import State
from lemonade.cache import Keys
from lemonade.tools.bench import Bench

default_beams = 1


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
            for count in tqdm.tqdm(range(warmup_iterations), desc=f"{mode} warmup"):
                model.generate(
                    input_ids,
                    num_beams=num_beams,
                    max_new_tokens=target_output_tokens,
                    min_new_tokens=target_output_tokens,
                    early_stopping=early_stopping,
                    pad_token_id=tokenizer.eos_token_id,
                )
                report_progress_fn((count + 1) / (warmup_iterations + iterations))

            for count in tqdm.tqdm(range(iterations), desc=f"{mode} iterations"):
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

                # Only count an iteration if it produced enough tokens
                if token_len >= target_output_tokens:
                    per_iteration_result.append((latency, token_len))

                report_progress_fn(
                    (warmup_iterations + count + 1) / (warmup_iterations + iterations)
                )

        if not per_iteration_result:
            raise Bench.not_enough_tokens(target_output_tokens)

    return per_iteration_result


class HuggingfaceBench(Bench):
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

    @staticmethod
    def parser(parser: argparse.ArgumentParser = None, add_help: bool = True):
        # Allow inherited classes to initialize and pass in a parser, add parameters to it if so
        if parser is None:
            parser = __class__.helpful_parser(
                short_description="Benchmark a torch.nn.Module LLM",
                add_help=add_help,
            )

        parser = Bench.parser(parser)

        parser.add_argument(
            "--num-beams",
            required=False,
            type=int,
            default=default_beams,
            help=f"Number of beams for the LLM to use (default: {default_beams})",
        )

        return parser

    def get_prompt_str(self, state, token_length):
        """
        Returns a string with the prescribed token length.
        """
        model = state.model
        tokenizer = state.tokenizer
        test_prompt = "word " * (token_length - 2)
        input_ids = (
            tokenizer(test_prompt, return_tensors="pt")
            .to(device=model.device)
            .input_ids
        )
        test_token_length = input_ids.shape[1]
        delta = test_token_length - token_length
        if delta == 0:
            return test_prompt
        return "word " * max(token_length - 2 - delta, 0)

    def run_prompt(
        self,
        state: State,
        report_progress_fn,
        prompt: str,
        iterations: int,
        warmup_iterations: int,
        output_tokens: int,
        num_beams: int = default_beams,
    ) -> State:
        """
        We don't have access to the internal timings of generate(), so time to first
        token (TTFT, aka prefill latency) and token/s are calculated using the following formulae:
            prefill_latency = latency of generate(output_tokens=1)
            execution_latency = latency of generate(output_tokens=output_tokens)
            tokens_per_second = (new_tokens - 1) / (execution_latency - prefill_latency)
        """

        if self.first_run_prompt:
            if vars(state).get(Keys.MODEL) is None:
                raise ValueError(
                    f"{self.__class__.__name__} requires that a model be passed from another tool"
                )
            if (
                vars(state).get("num_beams")
                and vars(state).get("num_beams") != num_beams
            ):
                raise ValueError(
                    f"Number of beams was set to {vars(state).get('num_beams')} "
                    f"in a previous tool, but it is set to {num_beams} in "
                    "this tool. The values must be the same."
                )

            # Save benchmarking parameters
            state.save_stat("num_beams", num_beams)

        model = state.model
        tokenizer = state.tokenizer
        dtype = state.dtype

        # Generate the input_ids outside the benchmarking function to make sure
        # the same input_ids are used everywhere
        input_ids = (
            tokenizer(prompt, return_tensors="pt").to(device=model.device).input_ids
        )
        self.input_ids_len_list.append(input_ids.shape[1])

        prefill_report_progress_fn = lambda x: report_progress_fn(0.5 * x)

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
            report_progress_fn=prefill_report_progress_fn,
        )

        time_to_first_token_per_iteration = [
            latency for latency, _ in prefill_per_iteration_result
        ]
        mean_time_to_first_token = statistics.mean(time_to_first_token_per_iteration)
        self.mean_time_to_first_token_list.append(mean_time_to_first_token)
        self.prefill_tokens_per_second_list.append(
            input_ids.shape[1] / mean_time_to_first_token
        )
        try:
            self.std_dev_time_to_first_token_list.append(
                statistics.stdev(time_to_first_token_per_iteration)
            )
        except StatisticsError:
            # Less than 2 measurements
            self.std_dev_time_to_first_token_list.append(None)

        decode_report_progress_fn = lambda x: report_progress_fn(0.5 + 0.5 * x)

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
            report_progress_fn=decode_report_progress_fn,
        )

        execution_latency_per_iteration = [
            latency for latency, _ in decode_per_iteration_result
        ]
        token_len_per_iteration = [
            token_len for _, token_len in decode_per_iteration_result
        ]
        mean_execution_latency = statistics.mean(execution_latency_per_iteration)
        mean_decode_latency = mean_execution_latency - mean_time_to_first_token
        mean_token_len = statistics.mean(token_len_per_iteration)
        # Subtract 1 so that we don't count the prefill token
        self.token_generation_tokens_per_second_list.append(
            (mean_token_len - 1) / mean_decode_latency
        )
