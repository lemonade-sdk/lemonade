import logging
import os
import platform
import shutil
import sys
import threading
import time
import zipfile
from typing import Optional
import psutil
import subprocess
import requests
import lemonade.common.build as build
import lemonade.common.printing as printing
from lemonade.tools.adapter import PassthroughTokenizer, ModelAdapter
from lemonade.common.system_info import get_system_info
from dotenv import set_key, load_dotenv

LLAMA_VERSION_VULKAN = "b6510"
LLAMA_VERSION_ROCM = "b1066"
LLAMA_VERSION_METAL = "b6510"


def identify_rocm_arch_from_name(device_name: str) -> str | None:
    """
    Identify the appropriate ROCm target architecture based on the device name
    """
    device_name_lower = device_name.lower()
    if "radeon" not in device_name_lower:
        return None

    # Check iGPUs
    # STX Halo iGPUs (gfx1151 architecture)
    # Radeon 8050S Graphics / Radeon 8060S Graphics
    target_arch = None
    if any(halo_igpu in device_name_lower.lower() for halo_igpu in ["8050s", "8060s"]):
        return "gfx1151"

    # Check dGPUs
    # RDNA4 GPUs (gfx120X architecture)
    # AMD Radeon AI PRO R9700, AMD Radeon RX 9070 XT, AMD Radeon RX 9070 GRE,
    # AMD Radeon RX 9070, AMD Radeon RX 9060 XT
    if any(
        rdna4_gpu in device_name_lower.lower()
        for rdna4_gpu in ["r9700", "9060", "9070"]
    ):
        return "gfx120X"

    # RDNA3 GPUs (gfx110X architecture)
    # AMD Radeon PRO V710, AMD Radeon PRO W7900 Dual Slot, AMD Radeon PRO W7900,
    # AMD Radeon PRO W7800 48GB, AMD Radeon PRO W7800, AMD Radeon PRO W7700,
    # AMD Radeon RX 7900 XTX, AMD Radeon RX 7900 XT, AMD Radeon RX 7900 GRE,
    # AMD Radeon RX 7800 XT, AMD Radeon RX 7700 XT
    elif any(
        rdna3_gpu in device_name_lower.lower()
        for rdna3_gpu in ["7700", "7800", "7900", "v710"]
    ):
        return "gfx110X"

    return None


def identify_rocm_arch() -> str:
    """
    Identify the appropriate ROCm target architecture based on the device info
    Returns tuple of (architecture, gpu_type) where gpu_type is 'igpu' or 'dgpu'
    """

    # Check for integrated and discrete AMD GPUs
    system_info = get_system_info()
    amd_igpu = system_info.get_amd_igpu_device()
    amd_dgpu = system_info.get_amd_dgpu_devices()
    target_arch = None
    for gpu in [amd_igpu] + amd_dgpu:
        if gpu.get("available") and gpu.get("name"):
            target_arch = identify_rocm_arch_from_name(gpu["name"].lower())
            if target_arch:
                break

    return target_arch


def identify_hip_id() -> str:
    """
    Identify the HIP ID
    """
    # Get HIP devices
    hip_devices = get_hip_devices()
    logging.debug(f"HIP devices found: {hip_devices}")
    if len(hip_devices) == 0:
        raise ValueError("No HIP devices found when identifying HIP ID")

    # Identify HIP devices that are compatible with our ROCm builds
    rocm_devices = []
    for device in hip_devices:
        device_id, device_name = device
        if identify_rocm_arch_from_name(device_name):
            rocm_devices.append([device_id, device_name])
    logging.debug(f"ROCm devices found: {rocm_devices}")

    # If no ROCm devices are found, use the last HIP device
    # This might be needed in some scenarios where HIP reports generic device names
    # Example: "AMD Radeon Graphics" for STX Halo iGPU on Ubuntu 24.04
    if len(rocm_devices) == 0:
        rocm_devices = [hip_devices[-1]]
        logging.warning(
            "No ROCm devices found when identifying HIP ID. "
            f"Falling back to the following device: {rocm_devices[0]}"
        )
    elif len(rocm_devices) > 1:
        logging.warning(
            f"Multiple ROCm devices found when identifying HIP ID: {rocm_devices}"
            "The last device will be used."
        )

    # Select the last device
    device_selected = rocm_devices[-1]
    logging.debug(f"Selected ROCm device: {device_selected}")

    # Return the device ID
    return device_selected[0]


def get_llama_version(backend: str) -> str:
    """
    Select the appropriate llama.cpp version based on the backend
    """
    if backend == "rocm":
        return LLAMA_VERSION_ROCM
    elif backend == "vulkan":
        return LLAMA_VERSION_VULKAN
    elif backend == "metal":
        return LLAMA_VERSION_METAL
    else:
        raise ValueError(
            f"Unsupported backend: {backend}. Supported: vulkan, rocm, metal"
        )


def get_llama_folder_path(backend: str):
    """
    Get path for llama.cpp platform-specific executables folder
    """
    return os.path.join(os.path.dirname(sys.executable), backend, "llama_server")


def get_llama_exe_path(exe_name: str, backend: str):
    """
    Get path to platform-specific llama-server executable
    """
    base_dir = get_llama_folder_path(backend)
    system = platform.system().lower()

    if system == "windows":
        return os.path.join(base_dir, f"{exe_name}.exe")
    else:  # Darwin/Linux/Ubuntu
        # Check if executable exists in build/bin subdirectory
        build_bin_path = os.path.join(base_dir, "build", "bin", exe_name)
        if os.path.exists(build_bin_path):
            return build_bin_path
        else:
            # Fallback to root directory
            return os.path.join(base_dir, exe_name)


def get_llama_server_exe_path(backend: str):
    """
    Get path to platform-specific llama-server executable
    """
    return get_llama_exe_path("llama-server", backend)


def get_llama_cli_exe_path(backend: str):
    """
    Get path to platform-specific llama-cli executable
    """
    return get_llama_exe_path("llama-cli", backend)


def get_llama_bench_exe_path(backend: str):
    """
    Get path to platform-specific llama-bench executable
    """
    return get_llama_exe_path("llama-bench", backend)


def get_version_txt_path(backend: str):
    """
    Get path to text file that contains version information
    """
    return os.path.join(get_llama_folder_path(backend), "version.txt")


def get_llama_installed_version(backend: str):
    """
    Gets version of installed llama.cpp
    Returns None if llama.cpp is not installed
    """
    version_txt_path = get_version_txt_path(backend)
    if os.path.exists(version_txt_path):
        with open(version_txt_path, "r", encoding="utf-8") as f:
            llama_installed_version = f.read()
            return llama_installed_version
    return None


def get_binary_url_and_filename(backend: str, target_arch: str = None):
    """
    Get the appropriate binary URL and filename based on platform and backend

    Args:
        backend: Backend to use
    """
    system = platform.system().lower()

    if backend == "rocm":

        # ROCm support from lemonade-sdk/llamacpp-rocm
        repo = "lemonade-sdk/llamacpp-rocm"
        version = LLAMA_VERSION_ROCM
        if system == "windows":
            filename = f"llama-{version}-windows-rocm-{target_arch}-x64.zip"
        elif system == "linux":
            filename = f"llama-{version}-ubuntu-rocm-{target_arch}-x64.zip"
        else:
            raise NotImplementedError(
                f"Platform {system} not supported for ROCm llamacpp. Supported: Windows, Ubuntu Linux"
            )

    elif backend == "vulkan":
        # Original Vulkan support from ggml-org/llama.cpp
        repo = "ggml-org/llama.cpp"
        version = LLAMA_VERSION_VULKAN
        if system == "windows":
            filename = f"llama-{version}-bin-win-vulkan-x64.zip"
        elif system == "linux":
            filename = f"llama-{version}-bin-ubuntu-vulkan-x64.zip"
        else:
            raise NotImplementedError(
                f"Platform {system} not supported for Vulkan llamacpp. Supported: Windows, Ubuntu Linux"
            )

    elif backend == "metal":
        # Metal support for macOS Apple Silicon from ggml-org/llama.cpp
        repo = "ggml-org/llama.cpp"
        version = LLAMA_VERSION_METAL
        if system == "darwin":
            if platform.machine().lower() in ["arm64", "aarch64"]:
                filename = f"llama-{version}-bin-macos-arm64.zip"
            else:
                raise NotImplementedError(
                    "Metal backend only supports Apple Silicon (ARM64) processors"
                )
        else:
            raise NotImplementedError(
                f"Platform {system} not supported for Metal llamacpp. Metal is only supported on macOS"
            )
    else:
        supported_backends = ["vulkan", "rocm", "metal"]
        raise NotImplementedError(
            f"Unsupported backend: {backend}. Supported backends: {supported_backends}"
        )

    url = f"https://github.com/{repo}/releases/download/{version}/{filename}"
    return url, filename


def validate_platform_support():
    """
    Validate platform support before attempting download
    """
    system = platform.system().lower()

    if system not in ["windows", "linux", "darwin"]:
        raise NotImplementedError(
            f"Platform {system} not supported for llamacpp. "
            "Supported: Windows, Ubuntu Linux, macOS"
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


def install_llamacpp(backend):
    """
    Installs or upgrades llama.cpp binaries if needed
    """

    # Exception will be thrown if platform is not supported
    validate_platform_support()

    version = get_llama_version(backend)

    # Get platform-specific paths at runtime
    llama_server_exe_dir = get_llama_folder_path(backend)
    llama_server_exe_path = get_llama_server_exe_path(backend)

    # Check whether the llamacpp install needs an upgrade
    version_txt_path = os.path.join(llama_server_exe_dir, "version.txt")
    backend_txt_path = os.path.join(llama_server_exe_dir, "backend.txt")

    logging.info(f"Using backend: {backend}")

    if os.path.exists(version_txt_path) and os.path.exists(backend_txt_path):
        with open(version_txt_path, "r", encoding="utf-8") as f:
            llamacpp_installed_version = f.read().strip()
        with open(backend_txt_path, "r", encoding="utf-8") as f:
            llamacpp_installed_backend = f.read().strip()

        if (
            llamacpp_installed_version != version
            or llamacpp_installed_backend != backend
        ):
            # Remove the existing install, which will trigger a new install
            # in the next code block
            shutil.rmtree(llama_server_exe_dir)
    elif os.path.exists(version_txt_path):
        # Old installation without backend tracking - remove to upgrade
        shutil.rmtree(llama_server_exe_dir)

    # Download llama.cpp server if it isn't already available
    if not os.path.exists(llama_server_exe_path):

        # Create the directory
        os.makedirs(llama_server_exe_dir, exist_ok=True)

        # Identify the target architecture (only needed for ROCm)
        target_arch = None
        if backend == "rocm":
            # Identify the target architecture
            target_arch = identify_rocm_arch()
            if not target_arch:
                system = platform.system().lower()
                if system == "linux":
                    hint = (
                        "Hint: If you think your device is supported, "
                        "running `sudo update-pciids` may help identify your hardware."
                    )
                else:
                    hint = ""
                raise ValueError(
                    "ROCm backend selected but no compatible ROCm target architecture found. "
                    "See https://github.com/lemonade-sdk/lemonade?tab=readme-ov-file#supported-configurations "
                    f"for supported configurations. {hint}"
                )

        # Direct download for Vulkan/ROCm
        llama_archive_url, filename = get_binary_url_and_filename(backend, target_arch)
        llama_archive_path = os.path.join(llama_server_exe_dir, filename)
        logging.info(f"Downloading llama.cpp server from {llama_archive_url}")

        with requests.get(llama_archive_url, stream=True) as r:
            r.raise_for_status()
            with open(llama_archive_path, "wb") as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)

        logging.info(f"Extracting {filename} to {llama_server_exe_dir}")
        if filename.endswith(".zip"):
            with zipfile.ZipFile(llama_archive_path, "r") as zip_ref:
                zip_ref.extractall(llama_server_exe_dir)

            # On Unix-like systems (macOS/Linux), make executables executable
            if platform.system().lower() in ["darwin", "linux"]:
                import stat

                # Find and make executable files executable
                for root, _, files in os.walk(llama_server_exe_dir):
                    for file in files:
                        file_path = os.path.join(root, file)
                        # Make files in bin/ directories executable
                        if "bin" in root.split(os.sep) or file in [
                            "llama-server",
                            "llama-simple",
                        ]:
                            try:
                                current_permissions = os.stat(file_path).st_mode
                                os.chmod(file_path, current_permissions | stat.S_IEXEC)
                                logging.debug(f"Made {file_path} executable")
                            except Exception as e:
                                raise RuntimeError(
                                    f"Failed to make {file_path} executable. This will prevent "
                                    f"llama-server from starting. Error: {e}"
                                )
        else:
            raise NotImplementedError(f"Unsupported archive format: {filename}")

        # Identify and set HIP ID
        if backend == "rocm":
            try:
                hip_id = identify_hip_id()
            except Exception as e:  # pylint: disable=broad-exception-caught
                hip_id = 0
                logging.warning(f"Error identifying HIP ID: {e}. Falling back to 0.")
            env_file_path = os.path.join(llama_server_exe_dir, ".env")
            set_key(env_file_path, "HIP_VISIBLE_DEVICES", str(hip_id))

        # Make executable on Linux - need to update paths after extraction
        if platform.system().lower() == "linux":
            # Re-get the paths since extraction might have changed the directory structure
            exe_paths = [
                (get_llama_server_exe_path(backend), "llama-server"),
                (get_llama_cli_exe_path(backend), "llama-cli"),
                (get_llama_bench_exe_path(backend), "llama-bench"),
            ]

            for exe_path, exe_name in exe_paths:
                if os.path.exists(exe_path):
                    os.chmod(exe_path, 0o755)
                    logging.info(f"Set executable permissions for {exe_path}")
                else:
                    logging.warning(
                        f"Could not find {exe_name} executable at {exe_path}"
                    )

        # Save version and backend info
        with open(version_txt_path, "w", encoding="utf-8") as vf:
            vf.write(version)
        with open(backend_txt_path, "w", encoding="utf-8") as bf:
            bf.write(backend)

        # Delete the archive file
        os.remove(llama_archive_path)


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
    checkpoint: str, variant: Optional[str], mmproj: str
) -> tuple[dict, list[str]]:
    """
    Identifies the GGUF model files in the repository that match the variant.
    """

    hint = """
    The CHECKPOINT:VARIANT scheme is used to specify model files in Hugging Face repositories.

    The VARIANT format can be one of several types:
    0. wildcard (*): download all .gguf files in the repo
    1. Full filename: exact file to download
    2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
    3. Quantization variant: find a single file ending with the variant name (case insensitive)
    4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)

    Examples:
    - "ggml-org/gpt-oss-120b-GGUF:*" -> downloads all .gguf files in repo
    - "unsloth/Qwen3-8B-GGUF:qwen3.gguf" -> downloads "qwen3.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF" -> downloads "Qwen3-30B-A3B-GGUF.gguf"
    - "unsloth/Qwen3-8B-GGUF:Q4_1" -> downloads "Qwen3-8B-GGUF-Q4_1.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF:Q4_0" -> downloads all files in "Q4_0/" folder
    """

    from huggingface_hub import list_repo_files

    repo_files = list_repo_files(checkpoint)
    sharded_files = []

    # (case 0) Wildcard, download everything
    if variant and variant == "*":
        sharded_files = [f for f in repo_files if f.endswith(".gguf")]

        # Sort to ensure consistent ordering
        sharded_files.sort()

        # Use first file as primary (this is how llamacpp handles it)
        variant_name = sharded_files[0]

    # (case 1) If variant ends in .gguf, use it directly
    elif variant and variant.endswith(".gguf"):
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


def resolve_local_gguf_model(
    checkpoint: str, variant: str, config_mmproj: str = None
) -> dict | None:
    """
    Attempts to resolve a GGUF model from the local HuggingFace cache.
    """
    from huggingface_hub.constants import HF_HUB_CACHE

    # Convert checkpoint to cache directory format
    if checkpoint.startswith("models--"):
        model_cache_dir = os.path.join(HF_HUB_CACHE, checkpoint)
    else:
        # This is a HuggingFace repo - convert to cache directory format
        repo_cache_name = checkpoint.replace("/", "--")
        model_cache_dir = os.path.join(HF_HUB_CACHE, f"models--{repo_cache_name}")

    # Check if the cache directory exists
    if not os.path.exists(model_cache_dir):
        return None

    gguf_file_found = None

    # If variant is specified, look for that specific file
    if variant:
        search_term = variant if variant.endswith(".gguf") else f"{variant}.gguf"

        for root, _, files in os.walk(model_cache_dir):
            if search_term in files:
                gguf_file_found = os.path.join(root, search_term)
                break

    # If no variant or variant not found, find any .gguf file (excluding mmproj)
    if not gguf_file_found:
        for root, _, files in os.walk(model_cache_dir):
            gguf_files = [
                f for f in files if f.endswith(".gguf") and "mmproj" not in f.lower()
            ]
            if gguf_files:
                gguf_file_found = os.path.join(root, gguf_files[0])
                break

    # If no GGUF file found, model is not in cache
    if not gguf_file_found:
        return None

    # Build result dictionary
    result = {"variant": gguf_file_found}

    # Search for mmproj file if provided
    if config_mmproj:
        for root, _, files in os.walk(model_cache_dir):
            if config_mmproj in files:
                result["mmproj"] = os.path.join(root, config_mmproj)
                break

    logging.info(f"Resolved local GGUF model: {result}")
    return result


def download_gguf(
    config_checkpoint: str, config_mmproj=None, do_not_upgrade: bool = False
) -> dict:
    """
    Downloads the GGUF file for the given model configuration from HuggingFace.

    This function downloads models from the internet. It does NOT check the local cache first.
    Callers should use resolve_local_gguf_model() if they want to check for existing models first.

    Args:
        config_checkpoint: Checkpoint identifier (file path or HF repo with variant)
        config_mmproj: Optional mmproj file to also download
        do_not_upgrade: If True, use local cache only without attempting to download updates

    Returns:
        Dictionary with "variant" (and optionally "mmproj") file paths
    """
    # Handle direct file path case - if the checkpoint is an actual file on disk
    if os.path.exists(config_checkpoint):
        result = {"variant": config_checkpoint}
        if config_mmproj:
            result["mmproj"] = config_mmproj
        return result

    # Parse checkpoint to extract base and variant
    # Checkpoint format: repo_name:variant (e.g., "unsloth/Qwen3-0.6B-GGUF:Q4_0")
    checkpoint, variant = parse_checkpoint(config_checkpoint)

    # Identify the GGUF model files in the repository that match the variant
    core_files, sharded_files = identify_gguf_models(checkpoint, variant, config_mmproj)

    # Download the files
    from lemonade.common.network import custom_snapshot_download

    snapshot_folder = custom_snapshot_download(
        checkpoint,
        allow_patterns=list(core_files.values()) + sharded_files,
        do_not_upgrade=do_not_upgrade,
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


# Function to read a stream (stdout or stderr) into a list
def stream_reader(stream, output_list):
    for line in iter(stream.readline, b""):
        decoded_line = line.decode().rstrip()
        output_list.append(decoded_line)
    stream.close()


def monitor_process_memory(pid, memory_data, interval=0.5):
    """Monitor memory usage of a process in a separate thread."""

    try:
        is_windows = platform.system() == "Windows"
        if is_windows:
            # We can only collect peak_wset in Windows
            process = psutil.Process(pid)
            while process.is_running():
                try:
                    mem_info = process.memory_info()
                    peak_wset = mem_info.peak_wset
                    if peak_wset is not None:
                        memory_data["peak_wset"] = peak_wset
                except psutil.NoSuchProcess:
                    break
                time.sleep(interval)
    except Exception as e:
        print(f"Error monitoring process: {e}")

    return memory_data


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
        bench_executable,
        reasoning=False,
        lib_dir=None,
        state=None,
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
        self.bench_executable = os.path.normpath(bench_executable)
        self.reasoning = reasoning
        self.lib_dir = lib_dir
        self.state = state

    def generate(
        self,
        input_ids: str,
        max_new_tokens: Optional[int] = None,
        temperature: float = 0.8,
        top_p: float = 0.95,
        top_k: int = 40,
        return_raw: bool = False,
        save_max_memory_used: bool = False,
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
            "--ctx-size",  # size of the prompt context, 0 = loaded from model
            str(self.context_size),
            "-n",  # number of tokens to predict, -1 = infinity, =2 - until context filled
            str(n_predict),
            "-t",  # number of threads to use during generation
            str(self.threads),
            "-p",
            prompt,
            "-b",  # logical maximum batch size
            "1",
            "-ub",  # physical maximum batch size
            "1",
            "--temp",
            str(temperature),
            "--top-p",
            str(top_p),
            "--top-k",
            str(top_k),
            "-e",  # process escape sequences
            "--no-conversation",  # disable conversation mode
            "--reasoning-format",  # leaves thoughts unparsed in message content
            "none",
        ]

        # If prompt exceeds 500 characters, then use a file
        if len(prompt) < 500:
            cmd += ["-p", prompt]
        else:
            # Create prompt file in cache directory
            prompt_file = os.path.join(
                build.output_dir(self.state.cache_dir, self.state.build_name),
                "prompt.txt",
            )
            with open(prompt_file, "w", encoding="utf-8") as file:
                file.write(prompt)
            cmd += ["-f", prompt_file]

        # Configure GPU layers: 99 for GPU, 0 for CPU-only
        ngl_value = "99" if self.device == "igpu" else "0"
        cmd = cmd + ["-ngl", ngl_value]

        cmd = [str(m) for m in cmd]

        # save llama-cli command
        self.state.llama_cli_cmd = getattr(self.state, "llama_cli_cmd", []) + [
            " ".join(cmd)
        ]

        try:
            # Set up environment with library path for Linux
            env = os.environ.copy()

            # Load environment variables from .env file in the executable directory
            exe_dir = os.path.dirname(self.executable)
            env_file_path = os.path.join(exe_dir, ".env")
            if os.path.exists(env_file_path):
                load_dotenv(env_file_path, override=True)
                env.update(os.environ)

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

            # Start memory monitoring in a separate thread
            if save_max_memory_used:
                memory_data = {}
                monitor_thread = threading.Thread(
                    target=monitor_process_memory,
                    args=(process.pid, memory_data),
                    daemon=True,
                )
                monitor_thread.start()

            # Communicate with the subprocess
            stdout, stderr = process.communicate(timeout=600)

            # save llama-cli command output with performance info to state
            # (can be viewed in state.yaml file in cache)
            self.state.llama_cli_stderr = getattr(
                self.state, "llama_cli_stderr", []
            ) + [
                [line for line in stderr.splitlines() if line.startswith("llama_perf_")]
            ]

            if process.returncode != 0:
                error_msg = f"llama.cpp failed with return code {process.returncode}.\n"
                error_msg += f"Command: {' '.join(cmd)}\n"
                error_msg += f"Error output:\n{stderr}\n"
                error_msg += f"Standard output:\n{stdout}"
                raise Exception(error_msg)

            if stdout is None:
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

            # Wait for monitor thread to finish and write peak_wset
            if save_max_memory_used:
                monitor_thread.join(timeout=2)
                self.peak_wset = memory_data.get("peak_wset", None)

            if return_raw:
                return [stdout, stderr]

            # Find where the prompt ends and the generated text begins
            prompt_found = False
            output_text = ""
            prompt_first_line = prompt.split("\n")[0]
            for line in stdout.splitlines():
                if prompt_first_line in line:
                    prompt_found = True
                if prompt_found:
                    line = line.replace("</s> [end of text]", "")
                    output_text = output_text + line

            if not prompt_found:
                raise Exception(
                    f"Could not find prompt '{prompt_first_line}' in llama.cpp output. "
                    "This usually means the model failed to process the prompt correctly.\n"
                    f"Raw output:\n{stdout}\n"
                    f"Stderr:\n{stderr}"
                )

            # Return list containing the generated text
            return [output_text]

        except Exception as e:
            error_msg = f"Failed to run llama-cli.exe command: {str(e)}\n"
            error_msg += f"Command: {' '.join(cmd)}"
            raise Exception(error_msg)

    def benchmark(self, prompt, iterations, output_tokens):
        """
        Runs the llama-bench.exe tool to measure TTFT and TPS
        """
        cmd = [
            self.bench_executable,
            "-m",
            self.model,
            "-r",
            iterations,
            "-p",
            str(prompt),
            "-n",
            output_tokens,
            "-t",
            self.threads if self.threads > 0 else 16,
            "-b",
            1,
            "-ub",
            1,
        ]
        cmd = [str(m) for m in cmd]

        # save llama-bench command
        self.state.llama_bench_cmd = " ".join(cmd)

        try:
            # Set up environment with library path for Linux
            env = os.environ.copy()

            # Load environment variables from .env file in the executable directory
            exe_dir = os.path.dirname(self.executable)
            env_file_path = os.path.join(exe_dir, ".env")
            if os.path.exists(env_file_path):
                load_dotenv(env_file_path, override=True)
                env.update(os.environ)

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

            # Start memory monitoring in a separate thread
            save_max_memory_used = platform.system() == "Windows"
            if save_max_memory_used:
                memory_data = {}
                monitor_thread = threading.Thread(
                    target=monitor_process_memory,
                    args=(process.pid, memory_data),
                    daemon=True,
                )
                monitor_thread.start()

            # Communicate with the subprocess
            stdout, stderr = process.communicate(timeout=600)

            # save llama-bench command output with performance info to state
            # (can be viewed in state.yaml file in cache)
            self.state.llama_bench_standard_output = stdout.splitlines()

            if process.returncode != 0:
                error_msg = (
                    f"llama-bench.exe failed with return code {process.returncode}.\n"
                )
                error_msg += f"Command: {' '.join(cmd)}\n"
                error_msg += f"Error output:\n{stderr}\n"
                error_msg += f"Standard output:\n{stdout}"
                raise Exception(error_msg)

            if stdout is None:
                error_msg = "No output received from llama-bench.exe process\n"
                error_msg += f"Error output:\n{stderr}\n"
                error_msg += f"Standard output:\n{stdout}"
                raise Exception(error_msg)

            # Parse information from llama-bench.exe output
            prompt_length = None
            pp_tps = None
            pp_tps_sd = None
            tg_tps = None
            tg_tps_sd = None

            for line in stdout.splitlines():
                # Parse TPS information
                if f"pp{prompt:d}" in line:
                    parts = line.split("|")
                    timings = parts[-2].strip().split(" ")
                    prompt_length = prompt
                    pp_tps = float(timings[0])
                    pp_tps_sd = float(timings[-1])
                if f"tg{output_tokens:d}" in line:
                    parts = line.split("|")
                    timings = parts[-2].strip().split(" ")
                    tg_tps = float(timings[0])
                    tg_tps_sd = float(timings[-1])

        except Exception as e:
            error_msg = f"Failed to run llama-bench.exe command: {str(e)}\n"
            error_msg += f"Command: {' '.join(cmd)}"
            raise Exception(error_msg)

        # Determine max memory used
        if save_max_memory_used:
            # Wait for monitor thread to finish
            monitor_thread.join(timeout=2)

            # Track memory usage concurrently
            peak_wset = memory_data.get("peak_wset", None)
        else:
            peak_wset = None

        return prompt_length, pp_tps, pp_tps_sd, tg_tps, tg_tps_sd, peak_wset


def get_hip_devices():
    """Get list of HIP devices with their IDs and names."""
    import ctypes
    import sys
    import os
    import glob
    from ctypes import c_int, POINTER
    from ctypes.util import find_library

    # Get llama.cpp path
    rocm_path = get_llama_folder_path("rocm")

    # Load HIP library
    hip_library_pattern = (
        "amdhip64*.dll" if sys.platform.startswith("win") else "libamdhip64*.so"
    )
    search_pattern = os.path.join(rocm_path, hip_library_pattern)
    matching_files = glob.glob(search_pattern)
    if not matching_files:
        raise RuntimeError(
            f"Could not find HIP runtime library matching pattern: {search_pattern}"
        )
    try:
        libhip = ctypes.CDLL(matching_files[0])
    except OSError:
        raise RuntimeError(
            f"Could not load HIP runtime library from {matching_files[0]}"
        )

    # Setup function signatures
    hipError_t = c_int
    hipDeviceProp_t = ctypes.c_char * 2048
    libhip.hipGetDeviceCount.restype = hipError_t
    libhip.hipGetDeviceCount.argtypes = [POINTER(c_int)]
    libhip.hipGetDeviceProperties.restype = hipError_t
    libhip.hipGetDeviceProperties.argtypes = [POINTER(hipDeviceProp_t), c_int]
    libhip.hipGetErrorString.restype = ctypes.c_char_p
    libhip.hipGetErrorString.argtypes = [hipError_t]

    # Get device count
    device_count = c_int()
    err = libhip.hipGetDeviceCount(ctypes.byref(device_count))
    if err != 0:
        logging.error(
            "hipGetDeviceCount failed:", libhip.hipGetErrorString(err).decode()
        )
        return []

    # Get device properties
    devices = []
    for i in range(device_count.value):
        prop = hipDeviceProp_t()
        err = libhip.hipGetDeviceProperties(ctypes.byref(prop), i)
        if err != 0:
            logging.error(
                f"hipGetDeviceProperties failed for device {i}:",
                libhip.hipGetErrorString(err).decode(),
            )
            continue

        # Extract device name from HIP device properties
        device_name = ctypes.string_at(prop, 256).decode("utf-8").rstrip("\x00")
        devices.append([i, device_name])

    return devices
