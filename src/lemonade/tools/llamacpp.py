import argparse
import os
import subprocess
from typing import Optional

from turnkeyml.state import State
from turnkeyml.tools import FirstTool

import turnkeyml.common.build as build
from .adapter import PassthroughTokenizer, ModelAdapter


def llamacpp_dir(state: State):
    return os.path.join(build.output_dir(state.cache_dir, state.build_name), "llamacpp")


class LlamaCppAdapter(ModelAdapter):
    unique_name = "llama-cpp-adapter"

    def __init__(self, executable, model, tool_dir, context_size, threads, temp):
        super().__init__()

        self.executable = executable
        self.model = model
        self.tool_dir = tool_dir
        self.context_size = context_size
        self.threads = threads
        self.temp = temp

    def generate(self, input_ids: str, max_new_tokens: Optional[int] = None):
        """
        Pass a text prompt into the llamacpp inference CLI.

        The input_ids arg here should receive the original text that
        would normally be encoded by a tokenizer.
        """

        cmd = [
            self.executable,
            "-e",
        ]

        optional_params = {
            "ctx-size": self.context_size,
            "n-predict": max_new_tokens,
            "threads": self.threads,
            "model": self.model,
            "prompt": input_ids,
            "temp": self.temp,
        }

        for flag, value in optional_params.items():
            if value is not None:
                cmd.append(f"--{flag} {value}")

        cmd = [str(m) for m in cmd]

        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )

        raw_output, raw_err = process.communicate()

        if process.returncode != 0:
            raise subprocess.CalledProcessError(
                process.returncode, process.args, raw_output, raw_err
            )

        prompt_found = False
        output_text = ""
        prompt_first_line = input_ids.split("\n")[0]
        for line in raw_output.splitlines():
            if prompt_first_line in line:
                prompt_found = True
            if prompt_found:
                line = line.replace("</s> [end of text]", "")
                output_text = output_text + line

        if not prompt_found:
            raise Exception("Prompt not found in result, this is a bug in lemonade.")

        return [output_text]


class LoadLlamaCpp(FirstTool):
    unique_name = "load-llama-cpp"

    def __init__(self):
        super().__init__(monitor_message="Running llama.cpp model")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Wrap Llamacpp models with an API",
            add_help=add_help,
        )

        parser.add_argument(
            "--executable",
            required=True,
            type=str,
            help="Executable name",
        )

        default_threads = 1
        parser.add_argument(
            "--threads",
            required=False,
            type=int,
            default=default_threads,
            help=f"Number of threads to use for generation (default: {default_threads})",
        )

        context_size = 512
        parser.add_argument(
            "--context-size",
            required=False,
            type=int,
            default=context_size,
            help=f"Context size of the prompt (default: {context_size})",
        )

        parser.add_argument(
            "--model-binary",
            required=False,
            help="Path to a .gguf model to use with benchmarking.",
        )

        parser.add_argument(
            "--temp",
            type=float,
            required=False,
            help="Temperature",
        )

        return parser

    def run(
        self,
        state: State,
        input: str = None,
        context_size: int = None,
        threads: int = None,
        executable: str = None,
        model_binary: str = None,
        temp: float = None,
    ) -> State:
        """
        Create a tokenizer instance and model instance in `state` that support:

        input_ids = tokenizer(prompt, return_tensors="pt").input_ids
        response = model.generate(input_ids, max_new_tokens=1)
        response_text = tokenizer.decode(response[0], skip_special_tokens=True).strip()
        """

        if executable is None:
            raise Exception(f"{self.__class__.unique_name} requires an executable")

        if input is not None and input != "":
            model_binary = input

        # Save execution parameters
        state.save_stat("context_size", context_size)
        state.save_stat("threads", threads)

        if model_binary is None:
            raise Exception(
                f"{self.__class__.unique_name} requires the preceding tool to pass a "
                "Llamacpp model, "
                "or for the user to supply a model with `--model-binary`"
            )

        state.model = LlamaCppAdapter(
            executable=executable,
            model=model_binary,
            tool_dir=llamacpp_dir(state),
            context_size=context_size,
            threads=threads,
            temp=temp,
        )
        state.tokenizer = PassthroughTokenizer()

        return state
