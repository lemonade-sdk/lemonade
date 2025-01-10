import argparse
import os
import subprocess
import statistics
import tqdm
from turnkeyml.state import State
from turnkeyml.tools import Tool
from lemonade.cache import Keys
import lemonade.tools.ort_genai.oga_bench as general
from lemonade.tools.llamacpp import LlamaCppAdapter


class LlamaCppBench(Tool):
    unique_name = "llama-cpp-bench"

    def __init__(self):
        super().__init__(monitor_message="Benchmarking LlamaCPP model")
        self.status_stats = [
            Keys.SECONDS_TO_FIRST_TOKEN,
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Benchmark a LLM via llama.cpp",
            add_help=add_help,
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

        context_size = 512
        parser.add_argument(
            "--context-size",
            required=False,
            type=int,
            default=context_size,
            help=f"Context size of the prompt (default: {context_size})",
        )

        output_tokens = 512
        parser.add_argument(
            "--output-tokens",
            required=False,
            type=int,
            default=output_tokens,
            help=f"Maximum number of output tokens the LLM should make (default: {output_tokens})",
        )

        default_iterations = 1
        parser.add_argument(
            "--iterations",
            "-i",
            required=False,
            type=int,
            default=default_iterations,
            help=f"Number of benchmarking iterations to run (default: {default_iterations})",
        )

        default_warmup_runs = 0
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
        context_size: int = len(general.default_prompt),
        output_tokens: int = general.default_output_tokens,
        iterations: int = general.default_iterations,
        warmup_iterations: int = general.default_warmup_runs,
    ) -> State:
        """
        Benchmark llama.cpp model that was loaded by LoadLlamaCpp.
        """

        # Save benchmarking parameters
        state.save_stat("prompt", prompt)
        state.save_stat("output_tokens", output_tokens)
        state.save_stat("context_size", context_size)
        state.save_stat("iterations", iterations)
        state.save_stat("warmup_iterations", warmup_iterations)

        if not hasattr(state, "model") or not isinstance(state.model, LlamaCppAdapter):
            raise Exception(
                f"{self.__class__.unique_name} requires a LlamaCppAdapter model to be "
                "loaded first. Please run load-llama-cpp before this tool."
            )

        iteration_tokens_per_second = []
        iteration_time_to_first_token = []

        for iteration in tqdm.tqdm(
            range(iterations), desc="iterations", disable=iterations < 2
        ):
            cmd = [
                state.model.executable,
                "-m",
                state.model.model,
                "--ctx-size",
                str(context_size),
                "-n",
                str(output_tokens),
                "-t",
                str(state.model.threads),
                "-p",
                prompt,
                "-e",
            ]

            cmd = [str(m) for m in cmd]

            try:
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    universal_newlines=True,
                    encoding="utf-8",
                    errors="replace",
                )

                raw_output, stderr = process.communicate()
                if process.returncode != 0:
                    error_msg = (
                        f"llama.cpp failed with return code {process.returncode}.\n"
                    )
                    error_msg += f"Command: {' '.join(cmd)}\n"
                    error_msg += f"Error output:\n{stderr}\n"
                    error_msg += f"Standard output:\n{raw_output}"
                    raise Exception(error_msg)

                if raw_output is None:
                    raise Exception("No output received from llama.cpp process")

            except Exception as e:
                error_msg = f"Failed to run llama.cpp command: {str(e)}\n"
                error_msg += f"Command: {' '.join(cmd)}"
                raise Exception(error_msg)

            ms_per_token = None
            time_to_first_token_ms = None
            for line in raw_output.splitlines():
                if "llama_perf_context_print:        eval time =" in line:
                    parts = line.split("(")[1].strip()
                    parts = parts.split(",")
                    ms_per_token = float(parts[0].split("ms per token")[0].strip())
                if "llama_perf_context_print: prompt eval time =" in line:
                    parts = line.split("=")[1].split("/")[0]
                    time_to_first_token_ms = float(parts.split("ms")[0].strip())

            if ms_per_token is None or time_to_first_token_ms is None:
                # Look in stderr as well since some versions of llama.cpp output timing there
                for line in stderr.splitlines():
                    if "llama_perf_context_print:        eval time =" in line:
                        parts = line.split("(")[1].strip()
                        parts = parts.split(",")
                        ms_per_token = float(parts[0].split("ms per token")[0].strip())
                    if "llama_perf_context_print: prompt eval time =" in line:
                        parts = line.split("=")[1].split("/")[0]
                        time_to_first_token_ms = float(parts.split("ms")[0].strip())

            if ms_per_token is None or time_to_first_token_ms is None:
                error_msg = "Could not find timing information in llama.cpp output.\n"
                error_msg += "Raw output:\n" + raw_output + "\n"
                error_msg += "Error output:\n" + stderr
                raise Exception(error_msg)

            # When output_tokens is set to 1 for accuracy tests, ms_per_token tends to 0
            # and causes a divide-by-zero error. Set tokens_per_second to 0 in such cases
            # as performance data for generating a few tokens is not relevant.
            tokens_per_second = 0
            if output_tokens > 5 and ms_per_token > 0:
                tokens_per_second = 1000 / ms_per_token
            time_to_first_token = time_to_first_token_ms / 1000

            if iteration > warmup_iterations - 1:
                iteration_tokens_per_second.append(tokens_per_second)
                iteration_time_to_first_token.append(time_to_first_token)

        token_generation_tokens_per_second = statistics.mean(
            iteration_tokens_per_second
        )
        mean_time_to_first_token = statistics.mean(iteration_time_to_first_token)

        state.save_stat(
            Keys.TOKEN_GENERATION_TOKENS_PER_SECOND, token_generation_tokens_per_second
        )
        state.save_stat(Keys.SECONDS_TO_FIRST_TOKEN, mean_time_to_first_token)

        return state
