"""Minimal llama-server download utility."""

import os
import sys
import platform
import zipfile
import logging
from pathlib import Path
import urllib.request


# llama.cpp version to download
LLAMA_VERSION = "b6510"  # Known stable version with Vulkan support


def get_download_url() -> str:
    """Get download URL for llama-server based on platform."""
    system = platform.system()

    if system == "Windows":
        # Windows Vulkan build
        return f"https://github.com/ggerganov/llama.cpp/releases/download/{LLAMA_VERSION}/llama-{LLAMA_VERSION}-bin-win-vulkan-x64.zip"
    elif system == "Linux":
        # Linux Vulkan build
        return f"https://github.com/ggerganov/llama.cpp/releases/download/{LLAMA_VERSION}/llama-{LLAMA_VERSION}-bin-ubuntu-x64.zip"
    elif system == "Darwin":
        # macOS Metal build (no Vulkan on macOS)
        return f"https://github.com/ggerganov/llama.cpp/releases/download/{LLAMA_VERSION}/llama-{LLAMA_VERSION}-bin-macos-arm64.zip"
    else:
        raise RuntimeError(f"Unsupported platform: {system}")


def get_llama_dir() -> Path:
    """Get directory where llama-server should be stored."""
    # Store in user's home directory
    home = Path.home()
    llama_dir = home / ".lemonade_lean" / "llama_server"
    llama_dir.mkdir(parents=True, exist_ok=True)
    return llama_dir


def get_llama_server_path() -> Path:
    """Get path to llama-server executable."""
    llama_dir = get_llama_dir()

    if platform.system() == "Windows":
        return llama_dir / "llama-server.exe"
    else:
        # Check both possible locations (root and build/bin)
        build_bin = llama_dir / "build" / "bin" / "llama-server"
        if build_bin.exists():
            return build_bin
        return llama_dir / "llama-server"


def download_llama_server():
    """Download and extract llama-server if not present."""
    llama_path = get_llama_server_path()

    # Check if already exists
    if llama_path.exists():
        logging.info(f"llama-server already installed at {llama_path}")
        return llama_path

    logging.info("llama-server not found, downloading...")

    # Get download URL
    url = get_download_url()
    llama_dir = get_llama_dir()
    zip_path = llama_dir / "llama.zip"

    try:
        # Download with progress
        logging.info(f"Downloading from {url}")

        def progress_hook(block_num, block_size, total_size):
            downloaded = block_num * block_size
            if total_size > 0:
                percent = min(100, downloaded * 100 / total_size)
                mb_downloaded = downloaded / (1024 * 1024)
                mb_total = total_size / (1024 * 1024)
                sys.stdout.write(
                    f"\rDownloading: {percent:.1f}% ({mb_downloaded:.1f}/{mb_total:.1f} MB)"
                )
                sys.stdout.flush()

        urllib.request.urlretrieve(url, zip_path, progress_hook)
        print()  # New line after progress

        # Extract
        logging.info("Extracting llama-server...")
        with zipfile.ZipFile(zip_path, "r") as zip_ref:
            zip_ref.extractall(llama_dir)

        # Remove zip file
        zip_path.unlink()

        # Make executable on Unix systems
        if platform.system() != "Windows":
            llama_path = get_llama_server_path()
            if llama_path.exists():
                os.chmod(llama_path, 0o755)
                # Also chmod build/bin/llama-server if it exists
                build_bin = llama_dir / "build" / "bin" / "llama-server"
                if build_bin.exists():
                    os.chmod(build_bin, 0o755)

        logging.info(f"llama-server installed successfully at {llama_path}")
        return llama_path

    except Exception as e:
        # Clean up on failure
        if zip_path.exists():
            zip_path.unlink()
        raise RuntimeError(f"Failed to download llama-server: {e}")


def ensure_llama_server() -> str:
    """Ensure llama-server is available, download if needed."""
    # First check if it's in PATH or env var
    if "LLAMA_SERVER_PATH" in os.environ:
        path = os.environ["LLAMA_SERVER_PATH"]
        if os.path.exists(path):
            return path

    # Check if in PATH
    system = platform.system()
    exe_name = "llama-server.exe" if system == "Windows" else "llama-server"

    # Try to find in PATH
    import shutil

    path_exe = shutil.which(exe_name)
    if path_exe:
        return path_exe

    # Download if not found
    llama_path = download_llama_server()

    if not llama_path.exists():
        raise RuntimeError("Failed to install llama-server")

    return str(llama_path)
