import argparse
import os
import matplotlib.pyplot as plt
import turnkeyml.common.build as build
from turnkeyml.state import State
from turnkeyml.tools import Tool
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter
from lemonade.cache import Keys

DEFAULT_GENERATE_PARAMS = {
    "do_sample": True,
    "top_k": 50,
    "top_p": 0.95,
    "temperature": 0.7,
}

DEFAULT_MAX_NEW_TOKENS = 512
DEFAULT_N_TRIALS = 1


def sanitize_string(input_string):
    return input_string.encode("charmap", "ignore").decode("charmap")


def sanitize_text(text):
    if isinstance(text, str):
        return sanitize_string(text)
    elif isinstance(text, list):
        return [sanitize_string(item) for item in text]
    else:
        raise TypeError("Input must be a string or a list of strings.")


def positive_int(x):
    """Conversion function for argparse"""
    i = int(x)
    if i < 1:
        raise ValueError("Non-positive values are not allowed")
    return i


class LLMPrompt(Tool):
    """
    Send a prompt to an LLM instance and print the response to the screen.

    Required input state:
        - state.model: LLM instance that supports the generate() method.
        - state.tokenizer: LLM tokenizer instance that supports the __call__() (ie, encode)
            and decode() methods.

    Output state produced:
        - "response": text response from the LLM.
    """

    unique_name = "llm-prompt"

    def __init__(self):
        super().__init__(monitor_message="Prompting LLM")

        self.status_stats = [
            Keys.PROMPT_TOKENS,
            Keys.PROMPT,
            Keys.RESPONSE_TOKENS,
            Keys.RESPONSE,
            Keys.RESPONSE_LENGTHS_HISTOGRAM,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Prompt an LLM and print the result",
            add_help=add_help,
        )

        parser.add_argument(
            "--prompt",
            "-p",
            help="Input prompt to the LLM. Two formats are supported. "
            "1) str: use a user-provided prompt string "
            "2) path/to/prompt.txt: load the prompt from a .txt file.",
            required=True,
        )

        parser.add_argument(
            "--max-new-tokens",
            "-m",
            default=DEFAULT_MAX_NEW_TOKENS,
            type=int,
            help=f"Maximum number of new tokens in the response "
            f"(default is {DEFAULT_MAX_NEW_TOKENS})",
        )

        parser.add_argument(
            "--n-trials",
            "-n",
            default=DEFAULT_N_TRIALS,
            type=positive_int,
            help=f"Number of responses the LLM will generate for the prompt "
            f"(useful for testing, default is {DEFAULT_N_TRIALS})",
        )

        return parser

    def parse(self, state: State, args, known_only=True) -> argparse.Namespace:
        """
        Helper function to parse CLI arguments into the args expected
        by run()
        """

        parsed_args = super().parse(state, args, known_only)

        # Decode prompt arg into a string prompt
        if parsed_args.prompt.endswith(".txt") and os.path.exists(parsed_args.prompt):
            with open(parsed_args.prompt, "r", encoding="utf-8") as f:
                parsed_args.prompt = f.read()
        else:
            # No change to the prompt
            pass

        return parsed_args

    def run(
        self,
        state: State,
        prompt: str = "Hello",
        max_new_tokens: int = DEFAULT_MAX_NEW_TOKENS,
        n_trials: int = DEFAULT_N_TRIALS,
    ) -> State:

        model: ModelAdapter = state.model
        tokenizer: TokenizerAdapter = state.tokenizer

        input_ids = tokenizer(prompt, return_tensors="pt").input_ids
        if isinstance(input_ids, (list, str)):
            # OGA models return a list of tokens
            # Our llama.cpp adapter returns a string
            len_tokens_in = len(input_ids)
        else:
            # HF models return a 2-D tensor
            len_tokens_in = input_ids.shape[1]

        len_tokens_out = []
        response_texts = []
        for trial in range(n_trials):
            if n_trials > 1:
                self.set_percent_progress(100.0 * trial / n_trials)

            # Get the response from the LLM, which may include the prompt in it
            response = model.generate(
                input_ids, max_new_tokens=max_new_tokens, **DEFAULT_GENERATE_PARAMS
            )

            # Flatten the input and response
            input_ids_array = (
                input_ids if isinstance(input_ids, (list, str)) else input_ids[0]
            )
            response_array = response if isinstance(response, str) else response[0]

            # Separate the prompt from the response
            len_tokens_out.append(len(response_array) - len_tokens_in)

            input_token = 0
            while (
                input_token < len_tokens_in
                and input_ids_array[input_token] == response_array[input_token]
            ):
                input_token += 1

            # Only decode the actual response (not the prompt)
            response_text = tokenizer.decode(
                response_array[input_token:], skip_special_tokens=True
            ).strip()
            response_texts.append(response_text)

        state.response = response_texts

        if n_trials == 1:
            len_tokens_out = len_tokens_out[0]
            response_texts = response_texts[0]
        else:
            self.set_percent_progress(None)

            # Plot data
            plt.figure()
            plt.hist(len_tokens_out, bins=20)
            plt.xlabel("Response Length (tokens)")
            plt.ylabel("Frequency")
            plt.title(f"Histogram of Response Lengths\n{state.build_name}")
            figure_path = os.path.join(
                build.output_dir(state.cache_dir, state.build_name),
                "response_lengths.png",
            )
            plt.savefig(figure_path)
            state.save_stat(Keys.RESPONSE_LENGTHS_HISTOGRAM, figure_path)

        state.save_stat(Keys.PROMPT_TOKENS, len_tokens_in)
        state.save_stat(Keys.PROMPT, prompt)
        state.save_stat(Keys.RESPONSE_TOKENS, len_tokens_out)
        state.save_stat(Keys.RESPONSE, sanitize_text(response_texts))

        return state
