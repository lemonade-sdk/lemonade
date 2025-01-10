import argparse
import os
import statistics
import tqdm
from turnkeyml.state import State
from turnkeyml.tools import Tool
from lemonade.cache import Keys
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter

default_iterations = 10
default_warmup_runs = 5
default_prompt = "Hello, I am conscious and"
default_beams = 1
default_output_tokens = 5


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


class OgaBench(Tool):
    """
    Benchmark any model that adheres to the ModelAdapter interface.

    Required input state:
        - MODEL: model instance to benchmark.
        - TOKENIZER: tokenizer instance used to generate inputs for the model.

    Output state produced: None
    """

    unique_name = "oga-bench"

    def __init__(self):
        super().__init__(monitor_message="Benchmarking LLM")

        self.status_stats = [
            Keys.SECONDS_TO_FIRST_TOKEN,
            Keys.PREFILL_TOKENS_PER_SECOND,
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND,
            Keys.PROMPT_TOKENS,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Benchmark any model that adheres to the lemonade standard",
            add_help=add_help,
        )

        parser.add_argument(
            "--prompt",
            "-p",
            required=False,
            default=default_prompt,
            help="Input prompt to the LLM. Three formats are supported. "
            f"1) integer (default: {default_prompt}): "
            "use a synthetic prompt with the specified length. "
            "2) str: use a user-provided prompt string "
            "3) path/to/prompt.txt: load the prompt from a text file.",
        )

        parser.add_argument(
            "--iterations",
            "-i",
            required=False,
            type=int,
            default=default_iterations,
            help=f"Number of benchmarking iterations to run (default: {default_iterations})",
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
            "--output-tokens",
            required=False,
            type=int,
            default=default_output_tokens,
            help=f"Number of new tokens the LLM should make (default: {default_output_tokens})",
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
        prompt: str = default_prompt,
        iterations: int = default_iterations,
        warmup_iterations: int = default_warmup_runs,
        output_tokens: int = default_output_tokens,
    ) -> State:

        model: ModelAdapter = state.model
        tokenizer: TokenizerAdapter = state.tokenizer

        input_ids = tokenizer(prompt, return_tensors="pt").input_ids
        if isinstance(input_ids, list):
            input_ids_len = len(input_ids)
        else:
            input_ids_len = input_ids.shape[1]
        per_iteration_time_to_first_token = []
        per_iteration_tokens_per_second = []

        # Don't capture time for warmup
        for _ in tqdm.tqdm(range(warmup_iterations), desc="warmup"):
            model.generate(input_ids, max_new_tokens=output_tokens)

        for _ in tqdm.tqdm(range(iterations), desc="iterations"):
            outputs = model.generate(input_ids, max_new_tokens=output_tokens)

            token_len = len(outputs[0]) - input_ids_len

            # Only count an iteration it produced enough tokens
            if token_len >= output_tokens:
                per_iteration_time_to_first_token.append(model.time_to_first_token)
                per_iteration_tokens_per_second.append(model.tokens_per_second)

        if not per_iteration_time_to_first_token or not per_iteration_tokens_per_second:
            raise not_enough_tokens(output_tokens)

        mean_time_to_first_token = statistics.mean(per_iteration_time_to_first_token)
        prefill_tokens_per_second = input_ids_len / mean_time_to_first_token
        token_generation_tokens_per_second = statistics.mean(
            per_iteration_tokens_per_second
        )

        state.save_stat(Keys.SECONDS_TO_FIRST_TOKEN, mean_time_to_first_token)
        state.save_stat(Keys.PREFILL_TOKENS_PER_SECOND, prefill_tokens_per_second)
        state.save_stat(
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND, token_generation_tokens_per_second
        )
        state.save_stat(Keys.PROMPT_TOKENS, input_ids_len)

        return state
