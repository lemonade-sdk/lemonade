import argparse
import os
import sys

import torch
from turnkeyml.state import State
from turnkeyml.tools import Tool
import turnkeyml.common.printing as printing
import turnkeyml.common.build as build
from lemonade_install.install import DEFAULT_QUARK_DIR


class QuarkLoad(Tool):
    """
    Load a model Quantized and exported using Quark.
    Required Input State:
        - state.model: Pretrained model instance to be quantized.
        - state.tokenizer: Tokenizer instance from Hugging Face.
    Output:
        - state of the loaded model

    See docs/quark.md for more details.
    """

    unique_name = "quark-load"

    def __init__(self):
        super().__init__(monitor_message="Load Quark Quantized model")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load a quantized model using Quark",
            add_help=add_help,
        )

        parser.add_argument(
            "--quant-scheme",
            type=str,
            required=True,
            default=None,
            help="Supported quantization schemes in Quark",
        )

        parser.add_argument(
            "--quant-algo",
            type=str,
            required=True,
            default=None,
            choices=["awq", "gptq", "autosmoothquant", None],
            help="Supported quantization algorithms in Quark",
        )

        parser.add_argument(
            "--torch-compile", action="store_true", help="Model torch compile"
        )

        parser.add_argument(
            "--safetensors-model-reload",
            action="store_true",
            help="Safetensors model reload",
        )

        parser.add_argument(
            "--safetensors-model-dir",
            default=None,
            help="Directory of safetensors model",
        )

        parser.add_argument(
            "--params-load", action="store_true", help="Model parameters load"
        )

        parser.add_argument("--json-path", help="Specify the path of saved json file")

        parser.add_argument(
            "--safetensors-path",
            default=None,
            help="Specify the path of saved safetensors file",
        )

        return parser

    def run(
        self,
        state: State,
        quant_scheme: str,
        quant_algo: str,
        torch_compile: bool = False,
        safetensors_model_reload: bool = False,
        safetensors_model_dir: str = None,
        params_load: bool = False,
        json_path: str = None,
        safetensors_path: str = None,
    ) -> State:
        """
        Executes the QuarkLoad process.
        Returns:
            State: The updated state after loading the model.
        Raises:
            Exception: If an error occurs during the QuarkLoad process.
        """

        try:
            if os.path.isdir(DEFAULT_QUARK_DIR):
                quark_llm_path = os.path.join(
                    DEFAULT_QUARK_DIR, "examples", "torch", "language_modeling"
                )
                sys.path.insert(0, quark_llm_path)
            else:
                raise FileNotFoundError(
                    f"The directory {DEFAULT_QUARK_DIR} does not exist. \
                        Please check your installation."
                )

            # Default load path specific to recipe
            # This will NOT work
            # The default path is now uniquely craeated with timestamp
            # Default load path will not work. Need to pass explicit load path
            model_export_path = os.path.join(
                build.output_dir(state.cache_dir, state.build_name),
                "exported_model",
                quant_scheme,
                quant_algo,
            )

            # Set default paths only if current values are None
            if safetensors_model_dir is None:
                safetensors_model_dir = model_export_path
            if safetensors_path is None:
                safetensors_path = os.path.join(model_export_path, "model.safetensors")
            printing.log_info("Loading model ...")
            if not params_load and not safetensors_model_reload:
                raise ValueError(
                    " Specify load format: 'params_load' or 'safetensors_model_reload'."
                )

            # Reload quantized model if specified
            from quark.torch import load_params, import_model_info

            if params_load:
                printing.log_info(
                    "Restoring quantized model from JSON/safetensors files"
                )
                model = load_params(
                    model,
                    json_path=json_path,
                    safetensors_path=safetensors_path,
                )
            elif safetensors_model_reload:
                printing.log_info(
                    "Restoring quantized model from quark_safetensors files"
                )
                model = import_model_info(model, model_info_dir=safetensors_model_dir)

            if torch_compile:
                printing.log_info("torch.compile...")
                model = torch.compile(model)

            state.model = model
            state.dtype = model.dtype

            printing.log_info("Quark Load process completed.")

        except Exception as e:
            printing.log_error(f"An error occurred during the QuarkLoad process: {e}")
            raise
        return state
