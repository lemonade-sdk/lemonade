from abc import ABC, abstractmethod
import argparse
import os
import platform
import psutil
from turnkeyml.state import State
from turnkeyml.tools import Tool
from lemonade.cache import Keys

default_iterations = 10
default_warmup_runs = 5
default_prompt_length = 64
default_output_tokens = 32
default_prompt = "Hello, I am conscious and"


class Bench(Tool, ABC):
    """
    Abstract parent class for tools that benchmark the performance of the generate()
    method of an LLM.
    """

    def __init__(self, monitor_message="Benchmarking LLM"):
        super().__init__(monitor_message)

        # The minimum set of statistics that a benchmark tool will produce
        # Inherited tools should append any additional statistics they generate to this list
        self.status_stats = [
            Keys.PROMPT_TOKENS,
            Keys.SECONDS_TO_FIRST_TOKEN,
            Keys.STD_DEV_SECONDS_TO_FIRST_TOKEN,
            Keys.PREFILL_TOKENS_PER_SECOND,
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND,
            Keys.MAX_MEMORY_USED_GBYTE,
        ]

        # Minimum per measurement statistics
        # Inherited tools should add additional lists for other per prompt statistics
        self.input_ids_len_list = []
        self.mean_time_to_first_token_list = []
        self.std_dev_time_to_first_token_list = []
        self.prefill_tokens_per_second_list = []
        self.token_generation_tokens_per_second_list = []
        self.max_memory_used_gb_list = []

        # Max memory used can only be measured on Windows systems
        self.save_max_memory_used = platform.system() == "Windows"

        # This is set to True only for the duration of the first call to run_prompt
        self.first_run_prompt = None

    @staticmethod
    def parser(parser: argparse.ArgumentParser = None, add_help: bool = True):
        # Allow inherited classes to initialize and pass in a parser, add parameters to it if so
        if parser is None:
            parser = __class__.helpful_parser(
                short_description="Benchmark an LLM", add_help=add_help
            )

        parser.add_argument(
            "--iterations",
            "-i",
            required=False,
            type=int,
            default=default_iterations,
            help="Number of benchmarking iterations to run (default: "
            f"{default_iterations})",
        )

        parser.add_argument(
            "--warmup-iterations",
            "-w",
            required=False,
            type=int,
            default=default_warmup_runs,
            help="Number of benchmarking iterations to use for cache warmup "
            "(the results of these iterations "
            f"are not included in the results; default: {default_warmup_runs})",
        )

        parser.add_argument(
            "--prompts",
            "-p",
            nargs="+",
            required=False,
            default=[str(default_prompt_length)],
            metavar="PROMPT",
            help="Input one or more prompts to the LLM. Three formats are supported. "
            "1) integer: use a synthetic prompt with the specified length "
            "2) str: use a user-provided prompt string "
            "3) path/to/prompt.txt: load the prompt from a text file. "
            f"(default: {default_prompt_length}) ",
        )

        parser.add_argument(
            "--output-tokens",
            required=False,
            type=int,
            default=default_output_tokens,
            help="Number of new tokens the LLM should make (default: "
            f"{default_output_tokens})",
        )

        return parser

    def get_prompt_str(self, _state, token_length):
        """
        Returns a string with approximately the prescribed token length.
        Note: Actual token length is dependent on the tokenizer.
        """
        return "word " * (token_length - 1)

    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:
        """
        Helper function to parse CLI arguments into the args expected by run()
        """

        parsed_args = super().parse(state, args, known_only)

        if parsed_args.prompts is None:
            parsed_args.prompts = [str(default_prompt_length)]

        # Decode prompt arg into a list of prompt strings
        prompt_strings = []
        for prompt_item in parsed_args.prompts:
            if prompt_item.isdigit():
                # Generate a prompt with the requested length
                token_length = int(prompt_item)
                prompt_strings.append(self.get_prompt_str(state, token_length))

            elif os.path.exists(prompt_item):
                with open(prompt_item, "r", encoding="utf-8") as f:
                    prompt_strings.append(f.read())

            else:
                # No change to the prompt
                prompt_strings.append(prompt_item)
        parsed_args.prompts = prompt_strings

        return parsed_args

    def run(
        self,
        state: State,
        prompts: list[str] = None,
        iterations: int = default_iterations,
        warmup_iterations: int = default_warmup_runs,
        output_tokens: int = default_output_tokens,
        **kwargs,
    ) -> State:
        """
        Args:
            - prompts: List of input prompts used as starting points for LLM text generation
            - iterations: number of benchmarking samples to take; results are
                reported as the median and mean of the samples.
            - warmup_iterations: subset of the iterations to treat as warmup,
                and not included in the results.
            - output_tokens: Number of new tokens LLM to create.
            - kwargs: Additional parameters used by bench tools
        """

        if prompts is None:
            prompts = ["word " * (default_prompt_length - 2)]
        elif isinstance(prompts, str):
            prompts = [prompts]

        state.save_stat("prompts", prompts)
        state.save_stat("iterations", iterations)
        state.save_stat("warmup_iterations", warmup_iterations)
        state.save_stat("output_tokens", output_tokens)

        counter = 0
        report_progress_fn = lambda x: self.set_percent_progress(
            100 * (counter + x) / len(prompts)
        )
        self.first_run_prompt = True
        for counter, prompt in enumerate(prompts):
            report_progress_fn(0)

            self.run_prompt(
                state,
                report_progress_fn,
                prompt,
                iterations,
                warmup_iterations,
                output_tokens,
                **kwargs,
            )
            self.first_run_prompt = False

            if self.save_max_memory_used:
                self.max_memory_used_gb_list.append(
                    psutil.Process().memory_info().peak_wset / 1024**3
                )

        self.set_percent_progress(None)
        self.save_stats(state)

        return state

    @abstractmethod
    def run_prompt(
        self,
        state,
        report_progress_fn,
        prompt,
        iterations,
        warmup_iterations,
        output_tokens,
        **kwargs,
    ):
        pass

    @staticmethod
    def get_item_or_list(lst):
        """
        If the list is just a single item then return the item, else return the list
        """
        if len(lst) == 1:
            return lst[0]
        else:
            return lst

    def save_stats(self, state):
        # Save performance data to stats
        state.save_stat(
            Keys.PROMPT_TOKENS, self.get_item_or_list(self.input_ids_len_list)
        )
        state.save_stat(
            Keys.SECONDS_TO_FIRST_TOKEN,
            self.get_item_or_list(self.mean_time_to_first_token_list),
        )
        if not all(
            element is None for element in self.std_dev_time_to_first_token_list
        ):
            state.save_stat(
                Keys.STD_DEV_SECONDS_TO_FIRST_TOKEN,
                self.get_item_or_list(self.std_dev_time_to_first_token_list),
            )
        state.save_stat(
            Keys.PREFILL_TOKENS_PER_SECOND,
            self.get_item_or_list(self.prefill_tokens_per_second_list),
        )
        state.save_stat(
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND,
            self.get_item_or_list(self.token_generation_tokens_per_second_list),
        )
        if self.save_max_memory_used:
            state.save_stat(
                Keys.MAX_MEMORY_USED_GBYTE,
                self.get_item_or_list(self.max_memory_used_gb_list),
            )

    @staticmethod
    def not_enough_tokens(output_tokens: int):
        """
        Raise an exception that explains why a benchmark did not produce any results
        """

        raise ValueError(
            "Your model was benchmarked, however none of the benchmarking "
            "iterations produced the requested amount of output tokens "
            f"(currently {output_tokens}), so "
            "the results have been discarded. You have the following options "
            "to solve this: \n"
            "1. Use the -p option to change the prompt to something that will "
            "produce more output tokens. For example, 'The extremely long "
            "story of my life, told in excruciating details is:' "
            "is an example of a prompt that will result in a lot of output. \n"
            "2. Set a lower value for --output-tokens to make it more likely "
            "that the model will produce enough. \n"
            "3. Set more verbose hyperparameters. \n"
            "4. Run more benchmarking iterations, to improve the chance of "
            "getting at least one with enough output tokens. \n"
        )
