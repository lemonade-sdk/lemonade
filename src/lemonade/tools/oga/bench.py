import argparse
import statistics
from statistics import StatisticsError
import psutil
from lemonade.state import State
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter
from lemonade.tools.bench import Bench


class OgaBench(Bench):
    """
    Benchmark any model that adheres to the ModelAdapter interface.

    Required input state:
        - MODEL: model instance to benchmark.
        - TOKENIZER: tokenizer instance used to generate inputs for the model.

    Output state produced: None
    """

    unique_name = "oga-bench"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Benchmark an LLM in onnxruntime-genai (OGA)",
            add_help=add_help,
        )

        parser = Bench.parser(parser)

        return parser

    def get_prompt_str(self, state, token_length):
        """
        Returns a string with the prescribed token length.
        """
        tokenizer: TokenizerAdapter = state.tokenizer
        test_prompt = "word " * (token_length - 1)
        input_ids = tokenizer(test_prompt, return_tensors="pt").input_ids
        test_token_length = len(input_ids)
        delta = test_token_length - token_length
        if delta == 0:
            return test_prompt
        return "word " * max(token_length - 1 - delta, 0)

    def run_prompt(
        self,
        state: State,
        report_progress_fn,
        prompt: str,
        iterations: int,
        warmup_iterations: int,
        output_tokens: int,
    ):

        model: ModelAdapter = state.model
        tokenizer: TokenizerAdapter = state.tokenizer

        input_ids = tokenizer(prompt, return_tensors="pt").input_ids
        self.input_ids_len_list.append(len(input_ids))
        per_iteration_time_to_first_token = []
        per_iteration_tokens_per_second = []

        # Don't capture time for warmup
        for count in range(warmup_iterations):
            _ = model.generate(input_ids, max_new_tokens=output_tokens)
            self.tokens_out_len_list.append(model.response_tokens)
            report_progress_fn((count + 1) / (warmup_iterations + iterations))

        for count in range(iterations):
            _ = model.generate(
                input_ids,
                max_new_tokens=output_tokens,
                min_new_tokens=output_tokens,
            )
            report_progress_fn(
                (warmup_iterations + count + 1) / (warmup_iterations + iterations)
            )

            self.tokens_out_len_list.append(model.response_tokens)

            # Only count an iteration if it produced enough tokens
            if model.response_tokens >= output_tokens:
                per_iteration_time_to_first_token.append(model.time_to_first_token)
                per_iteration_tokens_per_second.append(model.tokens_per_second)

        if not per_iteration_time_to_first_token or not per_iteration_tokens_per_second:
            raise Bench.not_enough_tokens(output_tokens)

        mean_time_to_first_token = statistics.mean(per_iteration_time_to_first_token)
        self.mean_time_to_first_token_list.append(mean_time_to_first_token)
        self.prefill_tokens_per_second_list.append(
            len(input_ids) / mean_time_to_first_token
        )
        self.token_generation_tokens_per_second_list.append(
            statistics.mean(per_iteration_tokens_per_second)
        )
        try:
            self.std_dev_time_to_first_token_list.append(
                statistics.stdev(per_iteration_time_to_first_token)
            )
        except StatisticsError:
            # Less than 2 measurements
            self.std_dev_time_to_first_token_list.append(None)
        try:
            self.std_dev_token_generation_tokens_per_second_list.append(
                statistics.stdev(per_iteration_tokens_per_second)
            )
        except StatisticsError:
            # Less than 2 measurements
            self.std_dev_token_generation_tokens_per_second_list.append(None)
        if self.save_max_memory_used:
            self.max_memory_used_gb_list.append(
                psutil.Process().memory_info().peak_wset / 1024**3
            )


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
