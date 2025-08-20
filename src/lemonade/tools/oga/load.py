# onnxruntime_genai is not lint-friendly yet and PyLint can't
# find any of the class methods
# pylint: disable=no-member

import argparse
import subprocess
import sys
import os
import json
import webbrowser
from fnmatch import fnmatch

from lemonade.state import State
from lemonade.tools import FirstTool
from lemonade.cache import Keys
import lemonade.common.status as status
import lemonade.common.printing as printing
from lemonade_install.install import (
    _get_ryzenai_version_info,
    SUPPORTED_RYZEN_AI_SERIES,
    NPU_DRIVER_DOWNLOAD_URL,
    REQUIRED_NPU_DRIVER_VERSION,
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


def _get_npu_driver_version():
    """
    Get the NPU driver version using PowerShell directly.
    Returns the driver version string or None if not found.
    """
    try:

        # Use PowerShell directly to avoid wmi issues in embedded Python environments
        powershell_cmd = [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            (
                "Get-WmiObject -Class Win32_PnPSignedDriver | "
                'Where-Object { $_.DeviceName -like "*NPU Compute Accelerator Device*" } | '
                "Select-Object -ExpandProperty DriverVersion"
            ),
        ]

        result = subprocess.run(
            powershell_cmd, capture_output=True, text=True, check=True, timeout=30
        )

        driver_version = result.stdout.strip()

        if driver_version and driver_version != "":
            return driver_version
        else:
            return None

    except Exception:  # pylint: disable=broad-except
        return None


def import_error_heler(e: Exception):
    """
    Print a helpful message in the event of an import error
    """
    raise ImportError(
        f"{e}\n Please install lemonade-sdk with "
        "one of the oga extras, for example:\n"
        "pip install lemonade-sdk[dev,oga-cpu]\n"
        "See https://lemonade-server.ai/install_options.html for details"
    )


def _open_driver_install_page():
    """
    Opens the driver installation page in the user's default web browser.
    """
    try:
        driver_page_url = "https://lemonade-server.ai/driver_install.html"
        printing.log_info(f"Opening driver installation guide: {driver_page_url}")
        webbrowser.open(driver_page_url)
    except Exception as e:  # pylint: disable=broad-except
        printing.log_info(f"Could not open browser automatically: {e}")
        printing.log_info(
            "Please visit https://lemonade-server.ai/driver_install.html "
            "for driver installation instructions."
        )


class OgaLoad(FirstTool):
    """
    Tool that loads an LLM in OnnxRuntime-GenAI for use with CPU or DirectML execution providers.

    Input: path to a checkpoint.
        Supported choices for cpu and igpu from HF model repository:
            LLM models on Huggingface supported by model_builder.  See documentation
            (https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev_cli/ort_genai_igpu.md)
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
        from lemonade.common.network import custom_snapshot_download

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
                snapshot_path = custom_snapshot_download(
                    checkpoint,
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
    def _setup_model_dependencies(full_model_path, device, ryzenai_version, oga_path):
        """
        Sets up model dependencies for hybrid and NPU inference by:
        1. Configuring the custom_ops_library path in genai_config.json.
        2. Adding DLL source directories to PATH for dependent DLL discovery.
        3. Check NPU driver version if required for device and ryzenai_version.
        """

        env_path = sys.prefix

        if "1.4.0" in ryzenai_version:
            if device == "npu":
                custom_ops_path = os.path.join(
                    oga_path, "libs", "onnxruntime_vitis_ai_custom_ops.dll"
                )
            else:
                custom_ops_path = os.path.join(oga_path, "libs", "onnx_custom_ops.dll")
        else:
            # For 1.5.0+, check NPU driver version for NPU and hybrid devices
            if device in ["npu", "hybrid"]:
                required_driver_version = REQUIRED_NPU_DRIVER_VERSION

                current_driver_version = _get_npu_driver_version()

                if not current_driver_version:
                    printing.log_warning(
                        f"NPU driver not found. {device.upper()} inference requires NPU driver "
                        f"version {required_driver_version}.\n"
                        "Please download and install the NPU Driver from:\n"
                        f"{NPU_DRIVER_DOWNLOAD_URL}\n"
                        "NPU functionality may not work properly."
                    )
                    _open_driver_install_page()

                elif current_driver_version != required_driver_version:
                    printing.log_warning(
                        f"Incorrect NPU driver version detected: {current_driver_version}\n"
                        f"{device.upper()} inference with RyzenAI 1.5.0 requires driver "
                        f"version {required_driver_version}.\n"
                        "Please download and install the correct NPU Driver from:\n"
                        f"{NPU_DRIVER_DOWNLOAD_URL}\n"
                        "NPU functionality may not work properly."
                    )
                    _open_driver_install_page()

            if device == "npu":
                # For 1.5.0, custom ops are in the conda environment's onnxruntime package
                custom_ops_path = os.path.join(
                    env_path,
                    "Lib",
                    "site-packages",
                    "onnxruntime",
                    "capi",
                    "onnxruntime_vitis_ai_custom_ops.dll",
                )
                dll_source_path = os.path.join(
                    env_path, "Lib", "site-packages", "onnxruntime", "capi"
                )
                required_dlls = ["dyn_dispatch_core.dll", "xaiengine.dll"]
            else:
                custom_ops_path = os.path.join(
                    env_path,
                    "Lib",
                    "site-packages",
                    "onnxruntime_genai",
                    "onnx_custom_ops.dll",
                )
                dll_source_path = os.path.join(
                    env_path, "Lib", "site-packages", "onnxruntime_genai"
                )
                required_dlls = ["libutf8_validity.dll", "abseil_dll.dll"]

            # Validate that all required DLLs exist in the source directory
            missing_dlls = []
            if not os.path.exists(custom_ops_path):
                missing_dlls.append(custom_ops_path)

            for dll_name in required_dlls:
                dll_source = os.path.join(dll_source_path, dll_name)
                if not os.path.exists(dll_source):
                    missing_dlls.append(dll_source)

            if missing_dlls:
                dll_list = "\n  - ".join(missing_dlls)
                raise RuntimeError(
                    f"Required DLLs not found for {device} inference:\n  - {dll_list}\n"
                    f"Please ensure your RyzenAI installation is complete and supports {device}."
                )

            # Add the DLL source directory to PATH
            current_path = os.environ.get("PATH", "")
            if dll_source_path not in current_path:
                os.environ["PATH"] = dll_source_path + os.pathsep + current_path

        # Update the model config with custom_ops_library path
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
    def _setup_npu_environment(ryzenai_version, oga_path):
        """
        Sets up environment for NPU flow of ONNX model and returns saved state to be restored
        later in cleanup.
        """
        if "1.5.0" in ryzenai_version:
            # For PyPI installation (1.5.0+), no environment setup needed
            return None
        elif "1.4.0" in ryzenai_version:
            # Legacy lemonade-install approach for 1.4.0
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
            return saved_state
        else:
            raise ValueError(f"Unsupported RyzenAI version: {ryzenai_version}")

    @staticmethod
    def _load_model_and_setup_state(
        state, full_model_path, checkpoint, trust_remote_code
    ):
        """
        Loads the OGA model from local folder and then loads the tokenizer.
        Will auto-detect if we're offline.
        """

        try:
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
            from transformers import AutoTokenizer
        except ImportError as e:
            import_error_heler(e)

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

    def _generate_model_for_oga(self, output_model_path, device, input_model_path):
        """
        Uses the model_generate tool to generate the model for OGA hybrid or npu targets.
        """
        try:
            import model_generate
        except ImportError as e:
            raise ImportError(
                f"{e}\nYou are trying to use a developer tool that may not be "
                "installed. Please install the required package using:\n"
                "pip install -e .[dev,oga-ryzenai] \
                    --extra-index-url https://pypi.amd.com/simple"
            )

        # Determine the appropriate flag based on the device type
        if device == "hybrid":
            device_flag = "hybrid"
        elif device == "npu":
            device_flag = "npu"
        else:
            raise ValueError(f"Unsupported device type for model generation: {device}")

        printing.log_info(
            f"Generating model for device: {device_flag}, \
            input: {input_model_path}, output: {output_model_path}"
        )

        try:
            if device_flag == "npu":
                model_generate.generate_npu_model(
                    input_model=input_model_path,
                    output_dir=output_model_path,
                    packed_const=False,
                )
            else:  # hybrid
                model_generate.generate_hybrid_model(
                    input_model=input_model_path,
                    output_dir=output_model_path,
                    script_option="jit_npu",
                    mode="bf16",
                    dml_only=False,
                )
        except Exception as e:
            raise RuntimeError(
                f"Failed to generate model for {device_flag} device. Error: {e}"
            ) from e

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
        do_not_upgrade: bool = False,
    ) -> State:
        from lemonade.common.network import (
            custom_snapshot_download,
            get_base_model,
            is_offline,
        )

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
            ryzenai_version, _ = _get_ryzenai_version_info(device)
            ryzen_ai_version_info = {"version": ryzenai_version}
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
                input_model_path = custom_snapshot_download(
                    checkpoint,
                    ignore_patterns=["*.md", "*.txt"],
                    local_files_only=offline or do_not_upgrade,
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
                            self._generate_model_for_oga(
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
                                self._generate_model_for_oga(
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
            # Get version information for NPU/Hybrid devices
            if device in ["hybrid", "npu"]:
                ryzenai_version, oga_path = _get_ryzenai_version_info(device)
            else:
                ryzenai_version, oga_path = None, None

            saved_env_state = None

            # Setup model dependencies for NPU/Hybrid devices
            if device in ["hybrid", "npu"]:
                self._setup_model_dependencies(
                    full_model_path, device, ryzenai_version, oga_path
                )

            try:
                if device == "npu":
                    saved_env_state = self._setup_npu_environment(
                        ryzenai_version, oga_path
                    )
                    # Set USE_AIE_RoPE based on model type
                    os.environ["USE_AIE_RoPE"] = (
                        "0" if "phi-" in checkpoint.lower() else "1"
                    )
                elif device == "hybrid":
                    saved_env_state = None

                self._load_model_and_setup_state(
                    state, full_model_path, checkpoint, trust_remote_code
                )
            finally:
                self._cleanup_environment(saved_env_state)

        return state


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
