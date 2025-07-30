import logging
import os
import platform
import shutil
import sys
import zipfile
from typing import Optional
import subprocess
import requests
import lemonade.common.printing as printing
from lemonade.tools.adapter import PassthroughTokenizer, ModelAdapter

LLAMA_VERSION = "b5902"


def get_llama_folder_path():
    """
    Get path for llama.cpp platform-specific executables folder
    """
    return os.path.join(os.path.dirname(sys.executable), "llamacpp")


def get_llama_exe_path(exe_name):
    """
    Get path to platform-specific llama-server executable
    """
    base_dir = get_llama_folder_path()
    if platform.system().lower() == "windows":
        return os.path.join(base_dir, f"{exe_name}.exe")
    else:  # Linux/Ubuntu
        # Check if executable exists in build/bin subdirectory (Current Ubuntu structure)
        build_bin_path = os.path.join(base_dir, "build", "bin", exe_name)
        if os.path.exists(build_bin_path):
            return build_bin_path
        else:
            # Fallback to root directory
            return os.path.join(base_dir, exe_name)


def get_llama_server_exe_path():
    """
    Get path to platform-specific llama-server executable
    """
    return get_llama_exe_path("llama-server")


def get_llama_cli_exe_path():
    """
    Get path to platform-specific llama-cli executable
    """
    return get_llama_exe_path("llama-cli")


def get_version_txt_path():
    """
    Get path to text file that contains version information
    """
    return os.path.join(get_llama_folder_path(), "version.txt")


def get_llama_installed_version():
    """
    Gets version of installed llama.cpp
    Returns None if llama.cpp is not installed
    """
    version_txt_path = get_version_txt_path()
    if os.path.exists(version_txt_path):
        with open(version_txt_path, "r", encoding="utf-8") as f:
            llama_installed_version = f.read()
            return llama_installed_version
    return None


def get_binary_url_and_filename(version):
    """
    Get the appropriate llama.cpp binary URL and filename based on platform
    """
    system = platform.system().lower()

    if system == "windows":
        filename = f"llama-{version}-bin-win-vulkan-x64.zip"
    elif system == "linux":
        filename = f"llama-{version}-bin-ubuntu-vulkan-x64.zip"
    else:
        raise NotImplementedError(
            f"Platform {system} not supported for llamacpp. Supported: Windows, Ubuntu Linux"
        )

    url = (
        f"https://github.com/ggml-org/llama.cpp/releases/download/{version}/{filename}"
    )
    return url, filename


def validate_platform_support():
    """
    Validate platform support before attempting download
    """
    system = platform.system().lower()

    if system not in ["windows", "linux"]:
        raise NotImplementedError(
            f"Platform {system} not supported for llamacpp. "
            "Supported: Windows, Ubuntu Linux"
        )

    if system == "linux":
        # Check if we're actually on Ubuntu/compatible distro and log a warning if not
        try:
            with open("/etc/os-release", "r", encoding="utf-8") as f:
                os_info = f.read().lower()
                if "ubuntu" not in os_info and "debian" not in os_info:
                    logging.warning(
                        "llamacpp binaries are built for Ubuntu. "
                        "Compatibility with other Linux distributions is not guaranteed."
                    )
        except (FileNotFoundError, PermissionError, OSError) as e:
            logging.warning(
                "Could not determine Linux distribution (%s). "
                "llamacpp binaries are built for Ubuntu.",
                str(e),
            )


def install_llamacpp():
    """
    Installs or upgrades llama.cpp binaries if needed
    """

    # Exception will be thrown if platform is not supported
    validate_platform_support()

    # Installation location for llama.cpp
    llama_folder_path = get_llama_folder_path()

    # Check whether the llamacpp install needs an upgrade
    if os.path.exists(llama_folder_path):
        if get_llama_installed_version() != LLAMA_VERSION:
            # Remove the existing install, which will trigger a new install
            # in the next code block
            shutil.rmtree(llama_folder_path)

    # Download llama.cpp server if it isn't already available
    if not os.path.exists(llama_folder_path):
        # Download llama.cpp server zip
        llama_zip_url, filename = get_binary_url_and_filename(LLAMA_VERSION)
        llama_zip_path = os.path.join(os.path.dirname(sys.executable), filename)
        logging.info(f"Downloading llama.cpp server from {llama_zip_url}")

        with requests.get(llama_zip_url, stream=True) as r:
            r.raise_for_status()
            with open(llama_zip_path, "wb") as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)

        # Extract zip
        logging.info(f"Extracting {llama_zip_path} to {llama_folder_path}")
        with zipfile.ZipFile(llama_zip_path, "r") as zip_ref:
            zip_ref.extractall(llama_folder_path)

        # Make executable on Linux - need to update paths after extraction
        if platform.system().lower() == "linux":
            # Re-get the paths since extraction might have changed the directory structure
            for updated_exe_path in [
                get_llama_server_exe_path(),
                get_llama_cli_exe_path(),
            ]:
                if os.path.exists(updated_exe_path):
                    os.chmod(updated_exe_path, 0o755)
                    logging.info(f"Set executable permissions for {updated_exe_path}")
                else:
                    logging.warning(
                        f"Could not find llama.cpp executable at {updated_exe_path}"
                    )

        # Save version.txt
        with open(get_version_txt_path(), "w", encoding="utf-8") as vf:
            vf.write(LLAMA_VERSION)

        # Delete zip file
        os.remove(llama_zip_path)
        logging.info("Cleaned up zip file")


def parse_checkpoint(checkpoint: str) -> tuple[str, str | None]:
    """
    Parse a checkpoint string that may contain a variant separated by a colon.

    For GGUF models, the format is "repository:variant" (e.g., "unsloth/Qwen3-0.6B-GGUF:Q4_0").
    For other models, there is no variant.

    Args:
        checkpoint: The checkpoint string, potentially with variant

    Returns:
        tuple: (base_checkpoint, variant) where variant is None if no colon is present
    """
    if ":" in checkpoint:
        base_checkpoint, variant = checkpoint.split(":", 1)
        return base_checkpoint, variant
    return checkpoint, None


def get_local_checkpoint_path(base_checkpoint, variant):
    """
    Returns the absolute path to a .gguf checkpoint file in the local HuggingFace hub.
    Also returns just .gguf filename.

    Checkpoint is one of the following types:
        1. Full filename: exact file to download
        2. Quantization variant: find a single file ending with the variant name (case insensitive)
        3. Folder name with subfolder that matches the variant name (case insensitive)

    """
    full_model_path = None
    model_to_use = None
    try:
        from lemonade.common.network import custom_snapshot_download

        snapshot_path = custom_snapshot_download(
            base_checkpoint,
            local_files_only=True,
        )

        full_model_path = None
        model_to_use = None

        if os.path.isdir(snapshot_path) and os.listdir(snapshot_path):

            snapshot_files = [filename for filename in os.listdir(snapshot_path)]

            if variant.endswith(".gguf"):
                # Variant is an exact file
                model_to_use = variant
                if variant in snapshot_files:
                    full_model_path = os.path.join(snapshot_path, variant)
                else:
                    raise ValueError(
                        f"The variant {variant} is not available locally in {snapshot_path}."
                    )

            else:
                # Variant is a quantization
                end_with_variant = [
                    file
                    for file in snapshot_files
                    if file.lower().endswith(f"{variant}.gguf".lower())
                ]
                if len(end_with_variant) == 1:
                    model_to_use = end_with_variant[0]
                    full_model_path = os.path.join(snapshot_path, model_to_use)
                elif len(end_with_variant) > 1:
                    raise ValueError(
                        f"Multiple .gguf files found for variant {variant}, "
                        f"but only one is allowed."
                    )
                else:
                    # Check whether the variant corresponds to a folder with
                    # sharded files (case insensitive)
                    quantization_folder = [
                        folder
                        for folder in snapshot_files
                        if folder.lower() == variant.lower()
                        and os.path.exists(os.path.join(snapshot_path, folder))
                        and os.path.isdir(os.path.join(snapshot_path, folder))
                    ]
                    if len(quantization_folder) == 1:
                        quantization_folder = os.path.join(
                            snapshot_path, quantization_folder[0]
                        )
                        sharded_files = [
                            f
                            for f in os.listdir(quantization_folder)
                            if f.endswith(".gguf")
                        ]
                        if not sharded_files:
                            raise ValueError(
                                f"No .gguf files found for variant {variant}."
                            )
                        else:
                            model_to_use = sharded_files[0]
                            full_model_path = os.path.join(
                                quantization_folder, model_to_use
                            )
                    elif len(quantization_folder) > 1:
                        raise ValueError(
                            f"Multiple checkpoint folder names match the variant {variant}."
                        )
                    else:
                        raise ValueError(f"No .gguf files found for variant {variant}.")
        else:
            raise ValueError(
                f"The checkpoint {base_checkpoint} is not a local checkpoint."
            )

    except Exception as e:  # pylint: disable=broad-exception-caught
        # Log any errors but continue with the original path
        printing.log_info(f"Error checking Hugging Face cache: {e}")

    return full_model_path, model_to_use


def identify_gguf_models(
    checkpoint: str, variant: str, mmproj: str
) -> tuple[dict, list[str]]:
    """
    Identifies the GGUF model files in the repository that match the variant.
    """

    hint = """
    The CHECKPOINT:VARIANT scheme is used to specify model files in Hugging Face repositories.

    The VARIANT format can be one of several types:
    1. Full filename: exact file to download
    2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
    3. Quantization variant: find a single file ending with the variant name (case insensitive)
    4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)

    Examples:
    - "unsloth/Qwen3-8B-GGUF:qwen3.gguf" -> downloads "qwen3.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF" -> downloads "Qwen3-30B-A3B-GGUF.gguf"
    - "unsloth/Qwen3-8B-GGUF:Q4_1" -> downloads "Qwen3-8B-GGUF-Q4_1.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF:Q4_0" -> downloads all files in "Q4_0/" folder
    """

    from huggingface_hub import list_repo_files

    repo_files = list_repo_files(checkpoint)
    sharded_files = []

    # (case 1) If variant ends in .gguf, use it directly
    if variant and variant.endswith(".gguf"):
        variant_name = variant
        if variant_name not in repo_files:
            raise ValueError(
                f"File {variant} not found in Hugging Face repository {checkpoint}. {hint}"
            )
    # (case 2) If no variant is provided, get the first .gguf file in the repository
    elif variant is None:
        all_variants = [
            f for f in repo_files if f.endswith(".gguf") and "mmproj" not in f
        ]
        if len(all_variants) == 0:
            raise ValueError(
                f"No .gguf files found in Hugging Face repository {checkpoint}. {hint}"
            )
        variant_name = all_variants[0]
    else:
        # (case 3) Find a single file ending with the variant name (case insensitive)
        end_with_variant = [
            f
            for f in repo_files
            if f.lower().endswith(f"{variant}.gguf".lower())
            and "mmproj" not in f.lower()
        ]
        if len(end_with_variant) == 1:
            variant_name = end_with_variant[0]
        elif len(end_with_variant) > 1:
            raise ValueError(
                f"Multiple .gguf files found for variant {variant}, but only one is allowed. {hint}"
            )
        # (case 4) Check whether the variant corresponds to a folder with
        # sharded files (case insensitive)
        else:
            sharded_files = [
                f
                for f in repo_files
                if f.endswith(".gguf") and f.lower().startswith(f"{variant}/".lower())
            ]

            if not sharded_files:
                raise ValueError(f"No .gguf files found for variant {variant}. {hint}")

            # Sort to ensure consistent ordering
            sharded_files.sort()

            # Use first file as primary (this is how llamacpp handles it)
            variant_name = sharded_files[0]

    core_files = {"variant": variant_name}

    # If there is a mmproj file, add it to the patterns
    if mmproj:
        if mmproj not in repo_files:
            raise ValueError(
                f"The provided mmproj file {mmproj} was not found in {checkpoint}."
            )
        core_files["mmproj"] = mmproj

    return core_files, sharded_files


def download_gguf(config_checkpoint, config_mmproj=None) -> dict:
    """
    Downloads the GGUF file for the given model configuration.

    For sharded models, if the variant points to a folder (e.g. Q4_0), all files in that folder
    will be downloaded but only the first file will be returned for loading.
    """

    # This code handles all cases by constructing the appropriate filename or pattern
    checkpoint, variant = parse_checkpoint(config_checkpoint)

    # Identify the GGUF model files in the repository that match the variant
    core_files, sharded_files = identify_gguf_models(checkpoint, variant, config_mmproj)

    # Download the files
    from lemonade.common.network import custom_snapshot_download

    snapshot_folder = custom_snapshot_download(
        checkpoint,
        allow_patterns=list(core_files.values()) + sharded_files,
    )

    # Ensure we downloaded all expected files
    for file in list(core_files.values()) + sharded_files:
        expected_path = os.path.join(snapshot_folder, file)
        if not os.path.exists(expected_path):
            raise ValueError(
                f"Hugging Face snapshot download for {config_checkpoint} "
                f"expected file {file} not found at {expected_path}"
            )

    # Return a dict of the full path of the core GGUF files
    return {
        file_name: os.path.join(snapshot_folder, file_path)
        for file_name, file_path in core_files.items()
    }


class LlamaCppTokenizerAdapter(PassthroughTokenizer):
    pass


class LlamaCppAdapter(ModelAdapter):
    def __init__(
        self,
        model,
        device,
        output_tokens,
        context_size,
        threads,
        executable,
        reasoning=False,
        lib_dir=None,
    ):
        super().__init__()

        self.model = os.path.normpath(model)
        self.device = device
        self.output_tokens = (
            output_tokens  # default value of max tokens to generate from a prompt
        )
        self.context_size = context_size
        self.threads = threads
        self.executable = os.path.normpath(executable)
        self.reasoning = reasoning
        self.lib_dir = lib_dir

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
        if self.reasoning:
            prompt += "<think>"
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
            "--reasoning-format",
            "none",
        ]

        # Configure GPU layers: 99 for GPU, 0 for CPU-only
        ngl_value = "99" if self.device == "igpu" else "0"
        cmd = cmd + ["-ngl", ngl_value]

        cmd = [str(m) for m in cmd]

        try:
            # Set up environment with library path for Linux
            env = os.environ.copy()
            if self.lib_dir and os.name != "nt":  # Not Windows
                current_ld_path = env.get("LD_LIBRARY_PATH", "")
                if current_ld_path:
                    env["LD_LIBRARY_PATH"] = f"{self.lib_dir}:{current_ld_path}"
                else:
                    env["LD_LIBRARY_PATH"] = self.lib_dir

            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                encoding="utf-8",
                errors="replace",
                env=env,
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

            # Parse information from llama.cpp output
            for line in stderr.splitlines():
                # Parse timing and token information
                #
                # Prompt processing time and length (tokens)
                # Sample: llama_perf_context_print: prompt eval time =      35.26 ms /
                #             3 tokens   (   11.75 ms per token,    85.09 tokens per second)
                #
                if "llama_perf_context_print: prompt eval time =" in line:
                    parts = line.split("=")[1].split()
                    time_to_first_token_ms = float(parts[0])
                    self.time_to_first_token = time_to_first_token_ms / 1000
                    self.prompt_tokens = int(parts[3])
                #
                # Response processing time and length (tokens)
                # Sample: llama_perf_context_print:        eval time =    1991.14 ms /
                #           63 runs   (   31.61 ms per token,    31.64 tokens per second)
                #
                if "llama_perf_context_print:        eval time =" in line:
                    parts = line.split("=")[1].split()
                    self.response_tokens = int(parts[3]) + 1  # include first token
                    response_time_ms = float(parts[0])
                    self.tokens_per_second = (
                        1000 * self.response_tokens / response_time_ms
                        if response_time_ms > 0
                        else 0
                    )

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
