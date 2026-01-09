import argparse
import time
import lemonade.common.status as status
from lemonade.state import State
from lemonade.tools import FirstTool
from lemonade.cache import Keys

DEFAULT_OUTPUT_TOKENS = 512


class FLMLoad(FirstTool):
    unique_name = "flm-load"

    def __init__(self):
        super().__init__(monitor_message="Starting FLM server and loading FLM model")

        self.status_stats = [
            Keys.DEVICE,
            Keys.BACKEND,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Wrap FLM models with an API",
            add_help=add_help,
        )

        parser.add_argument(
            "--force",
            default=False,
            type=bool,
            help="Force FLM model to be downloaded again (default: False)",
        )

        parser.add_argument(
            "--ctx-len",
            required=False,
            type=int,
            default=None,
            help="Context length that will override the models's default context length.  Minimum "
            "value for FLM is 512 and FLM automatically rounds up to the nearest power of 2.",
        )

        parser.add_argument(
            "--output-tokens",
            required=False,
            type=int,
            default=DEFAULT_OUTPUT_TOKENS,
            help=f"Maximum number of output tokens to generate (default: {DEFAULT_OUTPUT_TOKENS})",
        )

        return parser

    def run(
        self,
        state: State,
        input: str = "",
        force: bool = False,
        ctx_len: int = None,
        output_tokens: int = DEFAULT_OUTPUT_TOKENS,
    ) -> State:
        """
        Load an FLM model
        """
        from lemonade.tools.flm.utils import (
            install_flm,
            check_flm_version,
            FLMTokenizerAdapter,
            FLMAdapter,
        )

        # Install FLM if needed
        install_flm()

        # Input is an FLM model name, use it as the checkpoint value
        checkpoint = input
        state.checkpoint = checkpoint
        state.save_stat(Keys.CHECKPOINT, checkpoint)

        # Download the FLM model as needed
        state.model = FLMAdapter(input, output_tokens, state)
        state.model.download(force)
        state.tokenizer = FLMTokenizerAdapter()

        # Start the FLM server
        state.model.start_server(ctx_len=ctx_len)
        time.sleep(1)

        # Save initial stats
        state.save_stat(Keys.DEVICE, "npu")
        state.save_stat(Keys.BACKEND, "FastFlowLM")
        installed_flm_version, _ = check_flm_version()
        state.save_stat(Keys.FLM_VERSION_INFO, installed_flm_version)

        status.add_to_state(state=state, name=input, model=input)

        return state


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
