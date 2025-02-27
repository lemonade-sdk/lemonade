import argparse
import os
from typing import Optional
import subprocess
from turnkeyml.state import State
import turnkeyml.common.status as status
from turnkeyml.tools import FirstTool
from lemonade.tools.adapter import PassthroughTokenizer, ModelAdapter
from lemonade.cache import Keys
from lemonade.tools.huggingface_load import get_base_model


class LlamaCppAdapter(ModelAdapter):
    def __init__(self, model, output_tokens, context_size, threads, executable):
        super().__init__()

        self.model = os.path.normpath(model)
        self.output_tokens = output_tokens
        self.context_size = context_size
        self.threads = threads
        self.executable = os.path.normpath(executable)

    def generate(
        self,
        input_ids: str,
        max_new_tokens: Optional[int] = None,
        temperature: float = 0.8,
        top_p: float = 0.95,
        top_k: int = 40,
        return_raw: bool = False,
        **kwargs,  # pylint: disable=unused-argument
    ):
        """
        Pass a text prompt into the llamacpp inference CLI.

        The input_ids arg here should receive the original text that
        would normally be encoded by a tokenizer.

        Args:
            input_ids: The input text prompt
            max_new_tokens: Maximum number of tokens to generate
            temperature: Temperature for sampling (0.0 = greedy)
            top_p: Top-p sampling threshold
            top_k: Top-k sampling threshold
            return_raw: If True, returns the complete raw output including timing info
            **kwargs: Additional arguments (ignored)

        Returns:
            List containing a single string with the generated text, or raw output if
            return_raw=True
        """

        prompt = input_ids
        n_predict = max_new_tokens if max_new_tokens is not None else self.output_tokens

        cmd = [
            self.executable,
            "-m",
            self.model,
            "--ctx-size",
            str(self.context_size),
            "-n",
            str(n_predict),
            "-t",
            str(self.threads),
            "-p",
            prompt,
            "--temp",
            str(temperature),
            "--top-p",
            str(top_p),
            "--top-k",
            str(top_k),
            "-e",
            "-no-cnv",
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

            raw_output, stderr = process.communicate(timeout=600)
            if process.returncode != 0:
                error_msg = f"llama.cpp failed with return code {process.returncode}.\n"
                error_msg += f"Command: {' '.join(cmd)}\n"
                error_msg += f"Error output:\n{stderr}\n"
                error_msg += f"Standard output:\n{raw_output}"
                raise Exception(error_msg)

            if raw_output is None:
                raise Exception("No output received from llama.cpp process")

            # Parse timing information
            for line in raw_output.splitlines():
                if "llama_perf_context_print:        eval time =" in line:
                    parts = line.split("(")[1].strip()
                    parts = parts.split(",")
                    ms_per_token = float(parts[0].split("ms per token")[0].strip())
                    self.tokens_per_second = (
                        1000 / ms_per_token if ms_per_token > 0 else 0
                    )
                if "llama_perf_context_print: prompt eval time =" in line:
                    parts = line.split("=")[1].split("/")[0]
                    time_to_first_token_ms = float(parts.split("ms")[0].strip())
                    self.time_to_first_token = time_to_first_token_ms / 1000

            if return_raw:
                return [raw_output, stderr]

            # Find where the prompt ends and the generated text begins
            prompt_found = False
            output_text = ""
            prompt_first_line = prompt.split("\n")[0]
            for line in raw_output.splitlines():
                if prompt_first_line in line:
                    prompt_found = True
                if prompt_found:
                    line = line.replace("</s> [end of text]", "")
                    output_text = output_text + line

            if not prompt_found:
                raise Exception(
                    f"Could not find prompt '{prompt_first_line}' in llama.cpp output. "
                    "This usually means the model failed to process the prompt correctly.\n"
                    f"Raw output:\n{raw_output}\n"
                    f"Stderr:\n{stderr}"
                )

            # Return list containing the generated text
            return [output_text]

        except Exception as e:
            error_msg = f"Failed to run llama.cpp command: {str(e)}\n"
            error_msg += f"Command: {' '.join(cmd)}"
            raise Exception(error_msg)


class LoadLlamaCpp(FirstTool):
    unique_name = "load-llama-cpp"

    def __init__(self):
        super().__init__(monitor_message="Loading llama.cpp model")

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
            help="Path to the llama.cpp executable (e.g., llama-cli or llama-cli.exe)",
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

        output_tokens = 512
        parser.add_argument(
            "--output-tokens",
            required=False,
            type=int,
            default=output_tokens,
            help=f"Maximum number of output tokens the LLM should make (default: {output_tokens})",
        )

        parser.add_argument(
            "--model-binary",
            required=True,
            type=str,
            help="Path to a .gguf model file",
        )

        return parser

    def run(
        self,
        state: State,
        input: str = "",
        context_size: int = 512,
        threads: int = 1,
        output_tokens: int = 512,
        model_binary: Optional[str] = None,
        executable: str = None,
    ) -> State:
        """
        Load a llama.cpp model
        """

        if executable is None:
            raise Exception(f"{self.__class__.unique_name} requires an executable path")

        # Convert paths to platform-specific format
        executable = os.path.normpath(executable)

        if model_binary:
            model_to_use = os.path.normpath(model_binary)
        else:
            model_binary = input
            model_to_use = os.path.normpath(model_binary) if model_binary else None

            if not model_binary:
                model_to_use = state.get(Keys.MODEL)

        if model_to_use is None:
            raise Exception(
                f"{self.__class__.unique_name} requires the preceding tool to pass a "
                "Llamacpp model, "
                "or for the user to supply a model with `--model-binary`"
            )

        state.model = LlamaCppAdapter(
            model=model_to_use,
            output_tokens=output_tokens,
            context_size=context_size,
            threads=threads,
            executable=executable,
        )
        state.tokenizer = PassthroughTokenizer()

        # Save stats about the model
        state.save_stat(Keys.CHECKPOINT, model_to_use)

        # Get base model information if this is a converted HF model
        base_model = get_base_model(input)
        if base_model is not None:
            state.save_stat("base_model", base_model)

        status.add_to_state(state=state, name=input, model=model_to_use)

        return state
