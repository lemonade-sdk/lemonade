import argparse
import statistics
from statistics import StatisticsError
from lemonade.state import State
from lemonade.tools.tool import Tool
from lemonade.tools.flm.utils import FLMAdapter
from lemonade.tools.bench import (
    Bench,
    default_prompt_length,
    default_iterations,
    default_output_tokens,
    default_warmup_runs,
)


class FLMBench(Bench):
    """
    Benchmark an FLM model
    """

    unique_name = "flm-bench"

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Benchmark an FLM model",
            add_help=add_help,
        )

        parser = Bench.parser(parser)

        return parser

    def run_prompt(
        self,
        state: State,
        report_progress_fn,
        prompt: str,
        iterations: int,
        warmup_iterations: int,
        output_tokens: int,
    ):
        """
        Benchmark FLM model that was loaded by FLMLoad.
        """

        if self.first_run_prompt:

            if not hasattr(state, "model") or not isinstance(state.model, FLMAdapter):
                raise Exception(
                    f"{self.__class__.unique_name} requires an FLMAdapter model to be "
                    "loaded first. Please run flm-load before this tool."
                )
        model: FLMAdapter = state.model

        per_iteration_tokens_per_second = []
        per_iteration_time_to_first_token = []
        per_iteration_peak_wset = []

        for iteration in range(iterations + warmup_iterations):
            try:
                model.time_to_first_token = None
                model.tokens_per_second = None
                response = model.generate(
                    prompt,
                    max_new_tokens=output_tokens,
                    return_raw=True,
                    save_max_memory_used=self.save_max_memory_used,
                )
                self.tokens_out_len_list.append(model.response_tokens)

                if iteration > warmup_iterations - 1:
                    per_iteration_tokens_per_second.append(model.tokens_per_second)
                    per_iteration_time_to_first_token.append(model.time_to_first_token)
                    per_iteration_peak_wset.append(model.peak_wset)

                report_progress_fn((iteration + 1) / (warmup_iterations + iterations))

            except Exception as e:
                error_msg = f"Failed to run benchmark: {str(e)}"
                raise Exception(error_msg)

        self.input_ids_len_list.append(model.prompt_tokens)
        if all(value is None for value in per_iteration_tokens_per_second):
            self.mean_time_to_first_token_list.append(None)
            self.prefill_tokens_per_second_list.append(None)
            self.token_generation_tokens_per_second_list.append(None)
            self.std_dev_time_to_first_token_list.append(None)
            self.std_dev_token_generation_tokens_per_second_list.append(None)
        else:
            mean_time_to_first_token = statistics.mean(
                per_iteration_time_to_first_token
            )
            self.mean_time_to_first_token_list.append(mean_time_to_first_token)
            self.prefill_tokens_per_second_list.append(
                model.prompt_tokens / mean_time_to_first_token
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
            filtered_list = [
                item for item in per_iteration_peak_wset if item is not None
            ]
            mean_gb_used = (
                None
                if len(filtered_list) == 0
                else statistics.mean(filtered_list) / 1024**3
            )
            self.max_memory_used_gb_list.append(mean_gb_used)


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
