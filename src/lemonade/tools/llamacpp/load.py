import argparse
import os
import lemonade.common.printing as printing
import lemonade.common.status as status
from lemonade.state import State
from lemonade.tools import FirstTool
from lemonade.cache import Keys


class LoadLlamaCpp(FirstTool):
    unique_name = "llamacpp-load"

    def __init__(self):
        super().__init__(monitor_message="Loading llama.cpp model")

        self.status_stats = [
            Keys.DEVICE,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Wrap llama.cpp models with an API",
            add_help=add_help,
        )

        parser.add_argument(
            "-d",
            "--device",
            choices=["cpu", "igpu"],
            default="igpu",
            help="Which device to load the model on to (default: igpu)",
        )

        default_threads = -1
        parser.add_argument(
            "--threads",
            required=False,
            type=int,
            default=default_threads,
            help=f"Number of threads to use during generation (default: {default_threads})",
        )

        context_size = 4096
        parser.add_argument(
            "--context-size",
            required=False,
            type=int,
            default=context_size,
            help=f"Size of the prompt context (default: {context_size}. 0 = loaded from model)",
        )

        output_tokens = 512
        parser.add_argument(
            "--output-tokens",
            required=False,
            type=int,
            default=output_tokens,
            help=f"Maximum number of output tokens to generate (default: {output_tokens})",
        )

        parser.add_argument(
            "--reasoning",
            action="store_true",
            help="Set this flag to indicate the model is a reasoning model",
        )

        parser.add_argument(
            "--backend",
            choices=["vulkan", "rocm"],
            default="vulkan",
            help="Backend to use for llama.cpp (default: vulkan)",
        )

        return parser

    def run(
        self,
        state: State,
        input: str = "",
        device: str = "igpu",
        context_size: int = 512,
        threads: int = 1,
        output_tokens: int = 512,
        reasoning: bool = False,
        backend: str = "vulkan",
    ) -> State:
        """
        Load a llama.cpp model
        """

        from lemonade.common.network import is_offline
        from lemonade.tools.llamacpp.utils import (
            install_llamacpp,
            get_llama_cli_exe_path,
            get_llama_installed_version,
            parse_checkpoint,
            download_gguf,
            get_local_checkpoint_path,
            LlamaCppTokenizerAdapter,
            LlamaCppAdapter,
        )

        install_llamacpp(backend)

        # Check if input is a local folder containing a .GGUF model
        if os.path.isdir(input):
            # input is a local folder
            local_model_folder = os.path.abspath(input)
            checkpoint = "local_model"
            state.checkpoint = checkpoint
            state.save_stat(Keys.CHECKPOINT, checkpoint)
            state.save_stat(Keys.LOCAL_MODEL_FOLDER, local_model_folder)

            # See if there is a file ending in ".gguf" in this folder
            dir = os.listdir(input)
            gguf_files = [filename for filename in dir if filename.endswith(".gguf")]
            if len(gguf_files) == 0:
                raise ValueError(
                    f"The folder {input} does not contain a GGUF model file."
                )
            model_to_use = gguf_files[0]
            full_model_path = os.path.join(local_model_folder, model_to_use)

        else:
            # Input is a model checkpoint
            checkpoint = input
            state.checkpoint = checkpoint
            state.save_stat(Keys.CHECKPOINT, checkpoint)

            # Make sure that a variant is provided for the GGUF model
            base_checkpoint, variant = parse_checkpoint(checkpoint)
            if variant is None:
                raise ValueError(
                    "You are required to provide a 'variant' when "
                    "selecting a GGUF model. The variant is provided "
                    "as CHECKPOINT:VARIANT. For example: "
                    "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
                    "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:qwen2.5-coder-3b-instruct-q4_0.gguf"
                )

            # Auto-detect offline status
            offline = is_offline()
            if offline:
                printing.log_warning(
                    "Network connectivity to huggingface.co not detected. Running in offline mode."
                )
                full_model_path, model_to_use = get_local_checkpoint_path(
                    base_checkpoint, variant
                )
                if not full_model_path:
                    raise ValueError(
                        f"Model {checkpoint} is not available locally."
                        f"Cannot download in offline mode."
                    )

            else:

                snapshot_files = download_gguf(checkpoint)
                full_model_path = snapshot_files["variant"]
                model_to_use = os.path.basename(full_model_path)

        llama_cli_exe_path = get_llama_cli_exe_path(backend)
        printing.log_info(f"Using llama_cli for GGUF model: {llama_cli_exe_path}")

        # Get the directory containing the executable for shared libraries
        lib_dir = os.path.dirname(llama_cli_exe_path)

        # Pass the model and inputs into state
        state.model = LlamaCppAdapter(
            model=full_model_path,
            device=device,
            output_tokens=output_tokens,
            context_size=context_size,
            threads=threads,
            executable=llama_cli_exe_path,
            reasoning=reasoning,
            lib_dir=lib_dir,
        )
        state.tokenizer = LlamaCppTokenizerAdapter()
        state.device = device

        # Save initial stats
        state.save_stat(Keys.DEVICE, device)
        state.save_stat(
            Keys.LLAMA_CLI_VERSION_INFO, get_llama_installed_version(backend)
        )

        status.add_to_state(state=state, name=input, model=model_to_use)
        return state


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
