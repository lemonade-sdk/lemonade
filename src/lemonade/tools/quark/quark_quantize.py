import argparse
import os
import sys
from pathlib import Path

import torch
from transformers import AutoProcessor
from turnkeyml.state import State
from turnkeyml.tools import Tool
import turnkeyml.common.printing as printing
import turnkeyml.common.build as build
from lemonade_install.install import DEFAULT_QUARK_DIR


class QuarkQuantize(Tool):
    """
    Quantize a model using the Quark Quantization tool.

    This Tool performs the following steps:
    1. Downloads and extracts necessary resources from AMD Quark Web Page.
    2. Based on the target model, it prepares the model, tokenizer, and calibration data.
    3. Optionally quantizes, freezes, and exports the model.
    4. Optionally evaluates the model.

    Required Input State:
        - state.model: Pretrained model instance to be quantized.
        - state.tokenizer: Tokenizer instance from Hugging Face.
    Output:
        - Modifies `state` with quantized and optionally exported model.

    See docs/quark.md for more details.
    """

    unique_name = "quark-quantize"

    def __init__(self):
        super().__init__(monitor_message="Quark Quantizing model")

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Quantize a model using Quark",
            add_help=add_help,
        )
        parser.add_argument(
            "--device",
            default="cpu",
            choices=["cuda", "cpu"],
            help="Device for running the quantizer",
        )
        parser.add_argument("--multi-gpu", action="store_true")
        parser.add_argument(
            "--data-type",
            default="auto",
            choices=["auto", "float16", "bfloat16", "float32"],
            help="Input datatype of the model",
        )
        parser.add_argument(
            "--seq-len", type=int, default=512, help="Sequence length of data"
        )
        parser.add_argument(
            "--batch-size", type=int, default=1, help="Batch size for calibration."
        )
        parser.add_argument(
            "--num-fewshot",
            type=int,
            default=None,
            metavar="N",
            help="Number of examples in few-shot context",
        )
        parser.add_argument(
            "--output-dir", default=None, help="Output directory for exported model"
        )
        parser.add_argument(
            "--no-weight-matrix-merge",
            action="store_true",
            help="If set, merges onnx model and weight \
            together before export.\
            By default, for onnx export, spits out a model.onnx and a model.weights",
        )
        parser.add_argument(
            "--dataset",
            default="pileval",
            choices=[
                "pileval",
                "wikitext",
                "pileval_for_awq_benchmark",
                "wikitext_for_gptq_benchmark",
                "HuggingFaceH4/ultrachat_200k",
            ],
            help="Dataset for calibration",
        )
        parser.add_argument(
            "--num-calib-data",
            type=int,
            default=512,
            help="Number of samples for calibration.",
        )

        # See docs/quark.md for more details.
        parser.add_argument(
            "--quant-scheme",
            type=str,
            default=None,
            choices=[
                "w_fp8_a_fp8",
                "w_int4_per_channel_sym",
                "w_uint4_per_group_asym",
                "w_int4_per_group_sym",
                "w_uint4_a_bfloat16_per_group_asym",
                "w_int8_per_tensor_sym",
                "w_int8_per_group_sym",
                "w_uint8_per_group_asym",
                "w_int8_a_int8_per_tensor_sym",
                "w_int8_a_int8_per_tensor_sym_dynamic",
                "w_uint8_a_uint8_per_tensor_asym",
                "w_fp8_a_fp8_o_fp8",
                "w_mx_fp8",
                "w_mx_fp8_a_mx_fp8",
                "w_int8_a_int8_per_token_dynamic",
                "w_bfp16",
                "w_bfp16_a_bfp16",
                "w_mx6",
                "w_mx6_a_mx6",
                "w_fp8_per_channel_sym",
                "w_int4_per_channel_asym",
                "w_int4_per_group_asym",
                "w_uint4_per_group_sym",
                "w_uint4_per_channel_sym",
                "w_uint4_per_channel_asym",
                "w_int8_per_tensor_percentile",
                "w_int8_per_tensor_mse",
                "w_uint8_per_tensor_percentile",
                "w_uint8_per_tensor_mse",
                "w_mx_fp4_per_group_sym",
                "w_mx_fp6_e3m2_per_group_sym",
                "w_mx_fp6_e2m3_per_group_sym",
                "w_mx_int8_per_group_sym",
                "w_uint4_per_channel_a_int8_per_tensor",
                "w_uint4_per_group_a_int8_per_tensor",
                "w_bfp16_per_group_sym",
                None,
            ],
            help="Supported quantization schemes in Quark",
        )
        parser.add_argument(
            "--quant-algo",
            type=str,
            default=None,
            choices=["awq", "gptq", "autosmoothquant", None],
            help="Support quantization algorithms in Quark",
        )
        parser.add_argument(
            "--pre-optimization-config-file-path",
            type=str,
            default=None,
            help="The JSON file path of pre-optimization config",
        )
        parser.add_argument(
            "--quant-algo-config-file-path",
            type=str,
            default=None,
            help="The JSON file path of quantization algorithm config",
        )
        parser.add_argument(
            "--group-size",
            type=int,
            default=128,
            help="Group size for per_group quantization",
        )
        parser.add_argument(
            "--pack-method",
            type=str,
            default="reorder",
            choices=["order", "reorder"],
            help="Pack method for awq_export",
        )
        parser.add_argument(
            "--exclude-layers",
            type=str,
            nargs="*",
            default=None,
            help="List of layers to exclude from quantization.",
        )
        parser.add_argument(
            "--kv-cache-dtype",
            default=None,
            choices=["fp8", None],
            help="KV Cache dtype.",
        )
        parser.add_argument(
            "--pre-quantization-optimization",
            action="append",
            default=[],
            choices=["rotation", "smoothquant"],
            help="Pre Quantization Optimization.",
        )
        parser.add_argument(
            "--model-export",
            default=None,
            action="append",
            choices=[
                None,
                "onnx",
                "vllm_adopted_safetensors",
                "quark_safetensors",
                "gguf",
            ],
            help="Model export format",
        )
        parser.add_argument(
            "--custom-mode",
            default="quark",
            type=str,
            choices=["quark", "awq", "fp8"],
            help="Custom mode for export \
                This is especially relevant for npu/hybrid export",
        )
        parser.add_argument(
            "--torch-compile",
            action="store_true",
            help="Compile the quantized model using torch.compile",
        )
        parser.add_argument(
            "--params-save", action="store_true", help="Save model params"
        )
        parser.add_argument(
            "--save-dir",
            help="Directory to save model parameters as \
            safetensors or pth, in the case when --params_save is used.",
        )
        parser.add_argument(
            "--log-severity-level", type=int, default=3, help="DEBUG=1, INFO=2, ERROR=3"
        )
        parser.add_argument("--skip-quantization", action="store_true")

        return parser

    def run(self, state: State, **kwargs) -> State:
        """
        Executes the QuarkQuantize process.

        Args:
            state (State): The current state of the process, containing necessary
            information such as cache directory and build name.
            **kwargs: Additional keyword arguments that may include:
            - output_dir (str): Directory to save the output model.
            - safetensors_model_dir (str): Directory to save the safetensors model.
            - save_dir (str): Directory to save model parameters.
            - safetensors_path (str): Path to the safetensors model.
            - quant_algo (str): The quantization algorithm to use.
            - quant_algo_config_file_path (str): Path to the quantization algorithm
              configuration file.
            - model_dir (str): Directory of the model.
        Returns:
            State: The updated state after the quantization process.
        Raises:
            Exception: If an error occurs during the QuarkQuantize process
            and when installation path does not exist.
        """

        try:

            if os.path.isdir(DEFAULT_QUARK_DIR):
                quark_llm_path = os.path.join(
                    DEFAULT_QUARK_DIR, "examples", "torch", "language_modeling"
                )
                sys.path.extend([quark_llm_path])
            else:
                raise FileNotFoundError(
                    f"The directory {DEFAULT_QUARK_DIR} does not exist. \
                        Please check your installation."
                )
            model_build_path = os.path.join(
                build.output_dir(state.cache_dir, state.build_name)
            )
            model_export_path = os.path.join(
                model_build_path,
                "exported_model",
                kwargs.get("quant_scheme"),
                kwargs.get("quant_algo"),
            )
            # Set default paths only if current values are None
            if kwargs.get("model_dir") is None:
                kwargs["model_dir"] = model_build_path
            if kwargs.get("output_dir") is None:
                kwargs["output_dir"] = model_export_path
            if kwargs.get("save_dir") is None:
                kwargs["save_dir"] = os.path.join(model_export_path, "model_params")

            from llm_utils.model_preparation import get_model_type

            model_type = get_model_type(state.model)

            quant_algo = kwargs.get("quant_algo")
            kwargs["quant_algo_config_file_path"] = os.path.join(
                quark_llm_path,
                "llm_ptq",
                "models",
                model_type,
                f"{quant_algo}_config.json",
            )

            self._quantize(state, **kwargs)

        except Exception as e:
            printing.log_error(f"Error during the QuarkQuantize process: {e}")
            raise
        return state

    def _quantize(self, state: State, **kwargs) -> None:
        """
        Main quantization and export process.

        This method is responsible for:
        - Loading the model and tokenizer.
        - Preparing the calibration dataset.
        - Quantizing the model.
        - Optionally exporting, compiling, and evaluating the model.
        """

        model = state.model
        tokenizer = state.tokenizer

        # Importing quark utils after adding to sys.path
        from llm_utils.data_preparation import get_calib_dataloader
        from llm_utils.model_preparation import get_model_type
        from llm_ptq.configuration_preparation import get_config, get_export_config
        from quark.torch import ModelQuantizer, ModelExporter, save_params

        # 1. Load Model
        printing.log_info("Loading model ...")
        model_type = get_model_type(model)

        # [mllama specifics]
        if model_type == "mllama" and kwargs.get("model_export") is not None:
            processor = AutoProcessor.from_pretrained(kwargs.get("model_dir"))
            export_dir = Path(kwargs.get("output_dir"))
            export_dir.mkdir(parents=True, exist_ok=True)
            processor.save_pretrained(kwargs.get("output_dir"))

        # 2. Load dataset
        printing.log_info("Loading dataset ...")
        main_device = model.device if kwargs.get("multi_gpu") else kwargs.get("device")
        calib_dataloader = get_calib_dataloader(
            dataset_name=kwargs.get("dataset"),
            tokenizer=tokenizer,
            batch_size=1,
            num_calib_data=kwargs.get("num_calib_data"),
            seqlen=kwargs.get("seq_len"),
            device=main_device,
        )

        # 3. Quantize model
        if not kwargs.get("skip_quantization"):
            printing.log_info("Starting quantization process ...")
            args = argparse.Namespace(**kwargs)
            quant_config = get_config(args, model_type)
            quant_config.log_severity_level = kwargs.get("log_severity_level", 3)
            quantizer = ModelQuantizer(quant_config)
            model = quantizer.quantize_model(model, calib_dataloader)
            printing.log_info("Quantization completed.")

            if (
                kwargs.get("model_export") is not None
                or kwargs.get("params_save")
                or kwargs.get("torch_compile")
            ):
                printing.log_info("Freezing the quantized model ...")
                model = quantizer.freeze(model)

        # 4. Export model
        if kwargs.get("model_export") is not None:
            printing.log_info("Exporting the model ...")
            export_path = kwargs.get("output_dir")

            args = argparse.Namespace(**kwargs)
            export_config = get_export_config(args, model_type)
            exporter = ModelExporter(config=export_config, export_dir=export_path)
            if "quark_safetensors" in kwargs.get("model_export"):
                printing.log_info("Exporting quark native json and safetensors...")
                with torch.no_grad():
                    quant_config = get_config(args, model_type)
                    exporter.export_model_info(
                        model,
                        quant_config=quant_config,
                        tokenizer=tokenizer,
                        custom_mode=kwargs.get("custom_mode"),
                    )
            if "vllm_adopted_safetensors" in kwargs.get("model_export"):
                printing.log_info("Exporting vllm adopted json and safetensors...")
                with torch.inference_mode():
                    exporter.export_model_info(
                        model,
                        model_type=model_type,
                        model_dtype=state.dtype,
                        export_type="vllm-adopt",
                    )
            if "onnx" in kwargs.get("model_export"):
                printing.log_info("Exporting onnx graph...")
                with torch.inference_mode():
                    batch_iter = iter(calib_dataloader)
                    input_args = next(batch_iter)
                    if kwargs.get("quant_scheme") in [
                        "w_int4_per_channel_sym",
                        "w_uint4_per_group_asym",
                        "w_int4_per_group_sym",
                        "w_uint4_a_bfloat16_per_group_asym",
                    ]:
                        uint4_int4_flag = True
                    else:
                        uint4_int4_flag = False
                    exporter.export_onnx_model(
                        model, input_args, uint4_int4_flag=uint4_int4_flag
                    )
            if "gguf" in kwargs.get("model_export"):
                printing.log_info("Exporting gguf model...")
                with torch.inference_mode():
                    exporter.export_gguf_model(
                        model, kwargs.get("model_dir"), model_type
                    )

        # 6. [Optional] Compile model
        if kwargs.get("torch_compile"):
            printing.log_info("torch.compile...")
            model = torch.compile(model)

        # 7. Save model parameters
        if kwargs.get("params_save"):
            printing.log_info("Saving model parameters ...")
            save_params(model, model_type=model_type, export_dir=kwargs.get("save_dir"))

        state.model = model
        state.dtype = model.dtype
        printing.log_info("QuarkQuantize process completed.")
