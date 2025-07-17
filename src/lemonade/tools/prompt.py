import argparse
import os
import lemonade.common.build as build
import lemonade.common.printing as printing
from lemonade.state import State
from lemonade.tools import Tool
from lemonade.tools.adapter import ModelAdapter, TokenizerAdapter
from lemonade.cache import Keys

DEFAULT_GENERATE_PARAMS = {
    "do_sample": True,
    "top_k": 50,
    "top_p": 0.95,
    "temperature": 0.7,
}

DEFAULT_RANDOM_SEED = 1
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
            Keys.PROMPT_TEMPLATE,
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
            help="Input prompt to the LLM. Two formats are supported: "
            "1) str: use a user-provided prompt string, and "
            "2) path/to/prompt.txt: load the prompt from a .txt file.",
            required=True,
        )

        parser.add_argument(
            "--template",
            "-t",
            action="store_true",
            help="Insert the prompt into the model's chat template before processing.",
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

        parser.add_argument(
            "--random-seed",
            "-r",
            default=str(DEFAULT_RANDOM_SEED),
            help="Positive integer seed for random number generator used in "
            "sampling tokens "
            f"(default is {DEFAULT_RANDOM_SEED}). If the number of trials is "
            "greater than one, then the seed is incremented by one for each "
            "trial. Set to `None` for random, non-repeatable results.  This "
            "random seed behavior only applies to models loaded with "
            "`oga-load` or `huggingface-load`.",
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

        if parsed_args.random_seed == "None":
            parsed_args.random_seed = None
        else:
            parsed_args.random_seed = int(parsed_args.random_seed)

        return parsed_args

    def run(
        self,
        state: State,
        prompt: str = "Hello",
        max_new_tokens: int = DEFAULT_MAX_NEW_TOKENS,
        n_trials: int = DEFAULT_N_TRIALS,
        template: bool = False,
        random_seed: int = DEFAULT_RANDOM_SEED,
    ) -> State:

        import matplotlib.pyplot as plt

        model: ModelAdapter = state.model
        tokenizer: TokenizerAdapter = state.tokenizer

        # If template flag is set, then wrap prompt in template
        if template:
            # Embed prompt in model's chat template
            if not hasattr(tokenizer, "prompt_template"):
                printing.log_warning(
                    "Templates for this model type are not yet implemented."
                )
            elif tokenizer.chat_template:
                # Use the model's built-in chat template if available
                messages_dict = [{"role": "user", "content": prompt}]
                prompt = tokenizer.apply_chat_template(
                    messages_dict, tokenize=False, add_generation_prompt=True
                )
                state.save_stat(Keys.PROMPT_TEMPLATE, "Model-specific")
            else:
                # Fallback to a standardized template
                printing.log_info("No chat template found. Using default template.")
                prompt = f"<|user|>\n{prompt} <|end|>\n<|assistant|>"
                state.save_stat(Keys.PROMPT_TEMPLATE, "Default")

        input_ids = tokenizer(prompt, return_tensors="pt").input_ids

        len_tokens_out = []
        response_texts = []
        prompt_tokens = None  # will be determined in generate function
        for trial in range(n_trials):
            if n_trials > 1:
                self.set_percent_progress(100.0 * trial / n_trials)

            # Get the response from the LLM, which may include the prompt in it
            response = model.generate(
                input_ids,
                max_new_tokens=max_new_tokens,
                random_seed=random_seed,
                **DEFAULT_GENERATE_PARAMS,
            )

            # Increment random seed if not none
            if random_seed is not None:
                random_seed += 1

            # Flatten the input and response
            if isinstance(input_ids, (list, str)):
                input_ids_array = input_ids
            elif hasattr(input_ids, "shape") and len(input_ids.shape) == 1:
                # 1-D array from newer OGA versions - already flat
                input_ids_array = input_ids
            else:
                # 2-D tensor from HF models - take first row
                input_ids_array = input_ids[0]

            response_array = response if isinstance(response, str) else response[0]

            prompt_tokens = model.prompt_tokens
            len_tokens_out.append(model.response_tokens)

            # Remove the input from the response
            # (up to the point they diverge, which they should not)
            counter = 0
            len_input_ids = len(input_ids_array)
            while (
                counter < len_input_ids
                and input_ids_array[counter] == response_array[counter]
            ):
                counter += 1

            # Only decode the actual response (not the prompt)
            response_text = tokenizer.decode(
                response_array[counter:], skip_special_tokens=True
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

        state.save_stat(Keys.PROMPT_TOKENS, prompt_tokens)
        state.save_stat(Keys.PROMPT, prompt)
        state.save_stat(Keys.RESPONSE_TOKENS, len_tokens_out)
        state.save_stat(Keys.RESPONSE, sanitize_text(response_texts))

        return state


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
