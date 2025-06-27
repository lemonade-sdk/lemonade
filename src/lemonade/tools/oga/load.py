# onnxruntime_genai is not lint-friendly yet and PyLint can't
# find any of the class methods
# pylint: disable=no-member

import argparse
import os
import json
import shutil
from fnmatch import fnmatch
import subprocess


from lemonade.state import State
from lemonade.tools import FirstTool
import lemonade.common.status as status
import lemonade.common.printing as printing
from lemonade.cache import Keys
from lemonade_install.install import (
    get_ryzen_ai_version_info,
    get_oga_npu_dir,
    get_oga_hybrid_dir,
    SUPPORTED_RYZEN_AI_SERIES,
)


# ONNX Runtime GenAI models will be cached in this subfolder of the lemonade cache folder
oga_models_path = "oga_models"

# ONNX Runtime GenAI model builder tool uses this subfolder of the lemonade cache as its cache
oga_model_builder_cache_path = "model_builder"

# Mapping from processor to execution provider, used in pathnames and by model_builder
execution_providers = {
    "cpu": "cpu",
    "npu": "npu",
    "igpu": "dml",
    "hybrid": "hybrid",
    "cuda": "cuda",
}


def import_error_heler(e: Exception):
    """
    Print a helpful message in the event of an import error
    """
    raise ImportError(
        f"{e}\n Please install lemonade-sdk with "
        "one of the oga extras, for example:\n"
        "pip install lemonade-sdk[dev,oga-cpu]\n"
        "See https://lemonade_server.ai/install_options.html for details"
    )


class OgaLoad(FirstTool):
    """
    Tool that loads an LLM in OnnxRuntime-GenAI for use with CPU or DirectML execution providers.

    Input: path to a checkpoint.
        Supported choices for cpu and igpu from HF model repository:
            LLM models on Huggingface supported by model_builder.  See documentation
            (https://github.com/lemonade-sdk/lemonade/blob/main/docs/ort_genai_igpu.md)
            for supported models.
        Supported choices for npu from HF model repository:
            Models on Hugging Face that follow the "amd/**-onnx-ryzen-strix" pattern
        Local models for cpu, igpu, or npu:
            The specified checkpoint is converted to a local path, via mapping to lower case
            and replacing '/' with '_'.  If this model already exists in the 'models' folder
            of the lemonade cache and if it has a subfolder <device>-<dtype>, then this model
            will be used.  If the --force flag is used and the model is built with model_builder,
            then it will be rebuilt.



    Output:
        state.model: handle to a Huggingface-style LLM loaded on DirectML device
        state.tokenizer = Huggingface-style LLM tokenizer instance
        state.dtype = data type of the model on DirectML device
        state.checkpoint = name of the checkpoint used to load state.model

    Note: This tool expects the onnxruntime-genai-directml library to be pre-installed.
            If that library is not installed, this tool will not load.
    """

    unique_name = "oga-load"

    def __init__(self):
        super().__init__(monitor_message="Loading OnnxRuntime-GenAI model")

        self.status_stats = [
            Keys.DTYPE,
            Keys.DEVICE,
            Keys.LOCAL_MODEL_FOLDER,
        ]

    @staticmethod
    def parser(add_help: bool = True) -> argparse.ArgumentParser:
        parser = __class__.helpful_parser(
            short_description="Load model in onnxruntime-genai (OGA)",
            add_help=add_help,
        )

        parser.add_argument(
            "-ip",
            "--input_path",
            default="",
            help="the local huggingface model in your disk",
        )

        parser.add_argument(
            "-d",
            "--device",
            choices=["igpu", "npu", "cpu", "hybrid", "cuda"],
            default="igpu",
            help="Which device to load the model on to (default: igpu)",
        )

        parser.add_argument(
            "--dtype",
            choices=["int4", "fp16", "fp32"],
            required=True,
            help="Data type to load the model in",
        )

        parser.add_argument(
            "--int4-block-size",
            default=None,
            help="Specify the block_size for int4 quantization.",
            choices=[16, 32, 64, 128, 256],
            type=int,
        )

        parser.add_argument(
            "--force",
            action="store_true",
            help="Forces downloading of Hugging-Face model again (if changed).  Additionally for"
            " cpu and igpu devices only, forces model_builder to run again on the HF model"
            " (changed or not).",
        )

        parser.add_argument(
            "--download-only",
            action="store_true",
            help="Download the model if needed, but don't load it",
        )

        parser.add_argument(
            "--trust-remote-code",
            action="store_true",
            help="Set this flag to use models whose code is on the Hugging Face hub rather "
            "than natively in the OnnxRuntime Gen AI libraries.  Please review the model code "
            "in advance as this is a security risk.",
        )

        parser.add_argument(
            "--subfolder",
            default=None,
            help="Subfolder where model is located <LEMONADE CACHE>/oga_models/<MODELNAME>"
            "/<SUBFOLDER>, default is <EP for device>-<dtype>.  The EPs are: "
            f'{", ".join([value + " for " + key for key, value in execution_providers.items()])}.',
        )

        return parser

    @staticmethod
    def _validate_model_configuration(device, dtype, checkpoint):
        """
        Validate if the device, dtype, platform and checkpoint combination are consistent with
        HuggingFace checkpoint naming conventions and specifically for AMD models for NPU
        and hybrid flows.

        Returns True if device, dtype, and model are consistent.
        """

        hf_supported_models = {
            "cpu": {"int4": "*/*", "fp32": "*/*"},
            "igpu": {"int4": "*/*", "fp16": "*/*"},
            "npu": {"int4": "*/*"},
            "hybrid": {"int4": "*/*"},
            "cuda": {"int4": "*/*", "fp16": "*/*"},
        }

        hf_supported = (
            device in hf_supported_models
            and dtype in hf_supported_models[device]
            and fnmatch(checkpoint, hf_supported_models[device][dtype])
        )
        return hf_supported

    @staticmethod
    def _setup_model_paths(
        state, checkpoint, device, dtype, subfolder, int4_block_size
    ):
        """
        Determines and returns the following model path information for models produced by OGA
        model builder:

           full_model_path - Full path to where the OGA model files are stored.
           oga_models_subfolder - The subfolder of the oga_models folder where the model files
                are stored.  (<full_model_path> = <oga_models>/<oga_models_subfolder>)
                This subfolder is usually
                  <checkpoint_string>/<device>-<dtype>[-block-<int4_block_size]>
                but the if the argument subfolder is not None it will override the latter part
                of this path.
           model_exists_locally - True if full_model_path is a folder that contains files

        Note: Model files already in ONNX format on Hugging Face will be stored in the
            Hugging Face cache, not this folder.  The <oga_models> folder contains model
            files that have locally been quantized/converted to OGA format and any other
            models that have been manually added by the user.
        """
        from huggingface_hub import snapshot_download

        if subfolder is None:
            subfolder = f"{execution_providers[device]}-{dtype}"
            subfolder += (
                f"-block-{int4_block_size}"
                if dtype == "int4" and int4_block_size is not None
                else ""
            )

        # First, check in the lemonade oga_models cache
        oga_models_subfolder = os.path.join(
            checkpoint.replace("/", "_").lower(), subfolder
        )
        full_model_path = os.path.join(
            state.cache_dir, oga_models_path, oga_models_subfolder
        )
        model_exists_locally = os.path.isdir(full_model_path) and os.listdir(
            full_model_path
        )

        # If not found in lemonade cache, check in Hugging Face cache
        if not model_exists_locally:
            try:
                snapshot_path = snapshot_download(
                    repo_id=checkpoint,
                    local_files_only=True,
                )

                # Check if the snapshot contains ONNX files
                if os.path.isdir(snapshot_path) and os.listdir(snapshot_path):
                    is_onnx_model = any(
                        filename.endswith(".onnx")
                        for filename in os.listdir(snapshot_path)
                    )

                    if is_onnx_model:
                        # If the model is in HF cache and has ONNX files, use it
                        full_model_path = snapshot_path
                        model_exists_locally = True
                        printing.log_info(
                            f"Found ONNX model in Hugging Face cache: {full_model_path}"
                        )
            except Exception as e:  # pylint: disable=broad-exception-caught
                # Log any errors but continue with the original path
                printing.log_info(f"Error checking Hugging Face cache: {e}")

        return full_model_path, model_exists_locally

    @staticmethod
    def _update_hybrid_custom_ops_library_path(full_model_path):
        """
        Modifies the genai_config.json file in the hybrid model folder to set the custom_ops_library
        path to the location of the onnx_custom_ops.dll in the current environment.
        This is needed for hybrid inference.
        """
        oga_path, version = get_oga_hybrid_dir()

        if "1.3.0" in version:
            custom_ops_path = os.path.join(
                oga_path,
                "onnx_utils",
                "bin",
                "onnx_custom_ops.dll",
            )
        else:
            custom_ops_path = os.path.join(oga_path, "libs", "onnx_custom_ops.dll")

        # Insert the custom_ops_path into the model config file
        config_path = os.path.join(full_model_path, "genai_config.json")
        if os.path.exists(config_path):
            with open(config_path, "r", encoding="utf-8") as f:
                config = json.load(f)

            if (
                "model" in config
                and "decoder" in config["model"]
                and "session_options" in config["model"]["decoder"]
            ):
                config["model"]["decoder"]["session_options"][
                    "custom_ops_library"
                ] = custom_ops_path

            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(config, f, indent=4)

        else:
            printing.log_info(
                f"Model's `genai_config.json` not found in {full_model_path}"
            )

    @staticmethod
    def _is_preoptimized_model(input_model_path):
        """
        Checks if the 'custom_ops_library' field exists in the genai_config.json file
        to determine if this is a pre-optimized model for hybrid as well
        as NPU only.

        Args:
            input_model_path (str): Path to the input model directory.

        Returns:
            bool: True if 'custom_ops_library' exists, False otherwise.
        """
        config_path = os.path.join(input_model_path, "genai_config.json")
        if not os.path.exists(config_path):
            printing.log_info(f"Model's `genai_config.json` not found in {config_path}")
            return False

        with open(config_path, "r", encoding="utf-8") as f:
            config = json.load(f)
        if (
            "model" in config
            and "decoder" in config["model"]
            and "session_options" in config["model"]["decoder"]
        ):
            return "custom_ops_library" in config["model"]["decoder"]["session_options"]
        return False

    @staticmethod
    def _download_and_build_safetensors_model(
        checkpoint, device, dtype, full_model_path, int4_block_size, input_path, state
    ):
        """
        Uses OGA model builder to quantize safetensors format model and convert to ONNX
        format.  The model files are saved to the full_model_path folder.
        """

        try:
            import onnxruntime_genai.models.builder as model_builder
        except ImportError as e:
            import_error_heler(e)

        printing.log_info(f"Building {checkpoint} for {device} using {dtype}")
        extra_options = {}
        if int4_block_size is not None:
            extra_options["int4-block-size"] = int4_block_size
        try:
            model_builder.create_model(
                checkpoint,
                input_path,
                full_model_path,
                dtype,
                execution_providers[device],
                os.path.join(state.cache_dir, oga_model_builder_cache_path),
                **extra_options,
            )
        except NotImplementedError as e:
            raise NotImplementedError("[Model builder] " + str(e)) from e
        except OSError as e:
            raise ValueError("[Model builder] " + str(e)) from e

        return full_model_path

    @staticmethod
    def _setup_npu_environment():
        """
        Sets up environment for NPU flow of ONNX model and returns saved state to be restored
        later in cleanup.
        """
        oga_path, version = get_oga_npu_dir()

        if not os.path.exists(os.path.join(oga_path, "libs", "onnxruntime.dll")):
            raise RuntimeError(
                f"Cannot find libs/onnxruntime.dll in lib folder: {oga_path}"
            )

        # Save current state so they can be restored after inference.
        saved_state = {"cwd": os.getcwd(), "path": os.environ["PATH"]}

        # Setup NPU environment (cwd and path will be restored later)
        os.chdir(oga_path)
        os.environ["PATH"] = (
            os.path.join(oga_path, "libs") + os.pathsep + os.environ["PATH"]
        )
        if "1.3.0" in version:
            os.environ["DD_ROOT"] = ".\\bins"
            os.environ["DEVICE"] = "stx"
            os.environ["XLNX_ENABLE_CACHE"] = "0"

        return saved_state

    @staticmethod
    def _setup_hybrid_environment():
        """
        Sets up the environment for the Hybrid flow and returns saved state to be restored later
        in cleanup.
        """
        # Determine the Ryzen AI OGA version and hybrid artifacts path
        oga_path, version = get_oga_hybrid_dir()

        if "1.3.0" in version:
            dst_dll = os.path.join(
                oga_path,
                "onnx_utils",
                "bin",
                "DirectML.dll",
            )
            if not os.path.isfile(dst_dll):
                # Artifacts 1.3.0 has DirectML.dll in different subfolder, so copy it to the
                # correct place.  This should not be needed in later RAI release artifacts.
                src_dll = os.path.join(
                    oga_path,
                    "onnxruntime_genai",
                    "lib",
                    "DirectML.dll",
                )
                os.makedirs(os.path.dirname(dst_dll), exist_ok=True)
                shutil.copy2(src_dll, dst_dll)

        saved_state = None
        return saved_state

    @staticmethod
    def _load_model_and_setup_state(
        state, full_model_path, checkpoint, trust_remote_code
    ):
        """
        Loads the OGA model from local folder and then loads the tokenizer.
        Will auto-detect if we're offline.
        """

        try:
            from transformers import AutoTokenizer
            from lemonade.tools.oga.utils import OrtGenaiModel, OrtGenaiTokenizer
            from lemonade.common.network import is_offline
        except ImportError as e:
            import_error_heler(e)

        try:
            state.model = OrtGenaiModel(full_model_path)
        except Exception as e:
            if "invalid unordered_map<K, T>" in str(e):
                raise ValueError(
                    "Error initializing model: Invalid configuration detected.\n"
                    "Please check the following:\n"
                    f"1. Please check your model's config file in {full_model_path} "
                    "and ensure custom_ops_library points to the valid "
                    "onnx_custom_ops.dll path.\n"
                    "2. Make sure the NPU driver is loaded.\n"
                    "3. Make sure hybrid has been installed on a Ryzen AI "
                    f"{'or '.join(SUPPORTED_RYZEN_AI_SERIES)}-series processor."
                ) from e
            raise

        # Auto-detect offline mode
        offline = is_offline()

        try:
            # Always try to use local files first
            local_files_only = True

            hf_tokenizer = AutoTokenizer.from_pretrained(
                full_model_path,
                local_files_only=local_files_only,
                trust_remote_code=trust_remote_code,
            )
        except ValueError as e:
            if "trust_remote_code" in str(e):
                raise ValueError(
                    "This model requires you to execute code from the repo.  Please review it "
                    "and if you trust it, then use the `--trust-remote-code` flag with oga-load."
                )

            if offline and "Can't load tokenizer for" in str(e):
                raise ValueError(
                    f"Cannot load tokenizer for {checkpoint} in offline mode. "
                    f"The tokenizer files may not be available locally in {full_model_path}."
                )
            raise

        state.tokenizer = OrtGenaiTokenizer(
            state.model.model,
            hf_tokenizer,
        )

        status.add_to_state(state=state, name=checkpoint, model=checkpoint)

    @staticmethod
    def _cleanup_environment(saved_state):
        """
        Restores environment to its original state after inference is complete.
        """
        if saved_state:
            os.chdir(saved_state["cwd"])
            os.environ["PATH"] = saved_state["path"]

    def _generate_model_for_hybrid_or_npu(
        self, output_model_path, device, input_model_path
    ):
        """
        Uses a subprocess to run the 'model_generate' command for hybrid or npu devices.
        """

        # Determine the appropriate flag based on the device type
        if device == "hybrid":
            device_flag = "--hybrid"
        elif device == "npu":
            device_flag = "--npu"
        else:
            raise ValueError(f"Unsupported device type for model generation: {device}")

        command = [
            "model_generate",
            device_flag,
            output_model_path,  # Output model directory
            input_model_path,  # Input model directory
        ]

        printing.log_info(f"Running command: {' '.join(command)}")
        try:
            with open(self.logfile_path, "w", encoding="utf-8") as log_file:
                subprocess.run(
                    command, check=True, text=True, stdout=log_file, stderr=log_file
                )
        except FileNotFoundError as e:
            error_message = (
                "The 'model_generate' package is missing from your system. "
                "Ensure all required packages are installed. "
                "To install it, run the following command:\n\n"
                "    lemonade-install --ryzenai <target> --build-model\n"
            )
            raise RuntimeError(error_message) from e

    def run(
        self,
        state: State,
        input: str,
        input_path: str = "",
        device: str = "igpu",
        dtype: str = "int4",
        int4_block_size: int = None,
        force: bool = False,
        download_only: bool = False,
        trust_remote_code=False,
        subfolder: str = None,
    ) -> State:
        from huggingface_hub import snapshot_download
        from lemonade.common.network import get_base_model, is_offline

        # Auto-detect offline status
        offline = is_offline()
        if offline:
            printing.log_warning(
                "Network connectivity to huggingface.co not detected. Running in offline mode."
            )

        state.device = device
        state.dtype = dtype

        # Log initial stats
        state.save_stat(Keys.DTYPE, dtype)
        state.save_stat(Keys.DEVICE, device)
        if device in ["hybrid", "npu"]:
            ryzen_ai_version_info = get_ryzen_ai_version_info()
            state.save_stat(Keys.RYZEN_AI_VERSION_INFO, ryzen_ai_version_info)

        # Check if input is a local folder
        if os.path.isdir(input):
            # input is a local folder
            full_model_path = os.path.abspath(input)
            checkpoint = "local_model"
            state.checkpoint = checkpoint
            state.save_stat(Keys.CHECKPOINT, checkpoint)
            state.save_stat(Keys.LOCAL_MODEL_FOLDER, full_model_path)
            # See if there is a file ending in ".onnx" in this folder
            dir = os.listdir(input)
            has_onnx_file = any([filename.endswith(".onnx") for filename in dir])
            if not has_onnx_file:
                raise ValueError(
                    f"The folder {input} does not contain an ONNX model file."
                )
            if force:
                raise ValueError(
                    "Your input (-i, --input) points to a local folder, which is not "
                    "compatible with the force argument."
                )

        else:
            # input is a model checkpoint
            checkpoint = input
            state.checkpoint = checkpoint
            state.save_stat(Keys.CHECKPOINT, checkpoint)

            # Get base model information
            if not offline:
                base_model = get_base_model(checkpoint)
                if base_model is not None:
                    state.save_stat("base_model", base_model)

            # Setup paths
            full_model_path, model_exists_locally = self._setup_model_paths(
                state, checkpoint, device, dtype, subfolder, int4_block_size
            )

            # If in offline mode, we can only use locally available models
            if offline and not model_exists_locally:
                raise ValueError(
                    f"Model {checkpoint} is not available locally for {device} with {dtype}. "
                    f"Cannot download in offline mode. Check {full_model_path}"
                )

            # Handle download/build if needed
            if (not model_exists_locally) or force:
                if offline:
                    raise ValueError(
                        f"Cannot download or build model {checkpoint} in offline mode"
                    )

                # Validate configuration
                hf_supported = self._validate_model_configuration(
                    device, dtype, checkpoint
                )

                if not hf_supported:
                    raise ValueError(
                        "The (device, dtype, checkpoint) combination is not supported: "
                        f"({device}, {dtype}, {checkpoint})"
                    )
                input_model_path = snapshot_download(
                    repo_id=checkpoint,
                    ignore_patterns=["*.md", "*.txt"],
                    local_files_only=offline,
                )
                # Check if model is ONNX or safetensors
                is_onnx_model = any(
                    [
                        filename.endswith(".onnx")
                        for filename in os.listdir(input_model_path)
                    ]
                )
                is_preoptimized_onnx = is_onnx_model and self._is_preoptimized_model(
                    input_model_path
                )
                is_safetensors_model = any(
                    [
                        filename.endswith(".safetensors")
                        for filename in os.listdir(input_model_path)
                    ]
                )
                if not (is_onnx_model or is_safetensors_model):
                    raise ValueError(
                        f"The model {checkpoint} is not supported. "
                        "It does not contain ONNX or safetensors files."
                    )
                if device in ["npu", "hybrid"]:
                    if is_onnx_model:
                        if is_preoptimized_onnx:
                            # Use HuggingFace cache path as it is
                            full_model_path = input_model_path
                        else:
                            # If ONNX but not modified yet for Hybrid or NPU,
                            # needs further optimization
                            self._generate_model_for_hybrid_or_npu(
                                full_model_path,
                                device,
                                input_model_path,
                            )
                    elif is_safetensors_model:
                        config_path = os.path.join(input_model_path, "config.json")
                        if os.path.exists(config_path):
                            with open(config_path, "r", encoding="utf-8") as f:
                                config = json.load(f)
                            if "quantization_config" in config:
                                # If quantized, use subprocess to generate the model
                                self._generate_model_for_hybrid_or_npu(
                                    full_model_path, device, input_model_path
                                )
                            else:
                                raise ValueError(
                                    f"The safetensors model {checkpoint} is not quantized. "
                                    "Only quantized safetensors models are supported"
                                    " on npu or hybrid targets."
                                )
                        else:
                            raise ValueError(
                                f"config.json not found for safetensors model: {checkpoint}"
                            )
                    else:
                        raise ValueError(
                            f"Unsupported model type for checkpoint: {checkpoint}"
                        )
                else:
                    if is_onnx_model:
                        # Use HuggingFace cache path as it is
                        full_model_path = input_model_path
                    else:
                        self._download_and_build_safetensors_model(
                            checkpoint,
                            device,
                            dtype,
                            full_model_path,
                            int4_block_size,
                            input_path,
                            state,
                        )
                    state.save_stat(Keys.LOCAL_MODEL_FOLDER, full_model_path)

        # Load model if download-only argument is not set
        if not download_only:

            saved_env_state = None
            try:
                if device == "npu":
                    saved_env_state = self._setup_npu_environment()
                    # Set USE_AIE_RoPE based on model type
                    os.environ["USE_AIE_RoPE"] = (
                        "0" if "phi-" in checkpoint.lower() else "1"
                    )
                elif device == "hybrid":
                    saved_env_state = self._setup_hybrid_environment()
                    self._update_hybrid_custom_ops_library_path(full_model_path)

                self._load_model_and_setup_state(
                    state, full_model_path, checkpoint, trust_remote_code
                )
            finally:
                self._cleanup_environment(saved_env_state)

        return state


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
