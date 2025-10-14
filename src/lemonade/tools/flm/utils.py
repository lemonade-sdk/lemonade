"""
FLM (FastFlowLM) utilities for installation, version checking, and model management.
"""

import os
import logging
import subprocess
import tempfile
import time
from typing import List, Optional

import requests
from packaging.version import Version, InvalidVersion


def get_flm_latest_version() -> Optional[str]:
    """
    Get and return the latest FLM version from "https://github.com/FastFlowLM/FastFlowLM/tags"
    This uses the GitHub tags API.
    """
    url = "https://api.github.com/repos/FastFlowLM/FastFlowLM/tags"
    try:
        response = requests.get(url, timeout=10)
        response.raise_for_status()
        tags = response.json()
        if not tags:
            return None
        # Tags are sorted in reverse chronological order; find the first that looks like a version
        for tag in tags:
            tag_name = tag.get("name", "")
            # Accept tags of the form v0.9.10, 0.9.10, etc.
            if tag_name.startswith("v"):
                version_candidate = tag_name[1:]
            else:
                version_candidate = tag_name
            try:
                # validate it's a version string
                _ = Version(version_candidate)
                return version_candidate
            except InvalidVersion:
                continue
        return None
    except requests.exceptions.RequestException as e:
        logging.debug("Error retrieving latest FLM version: %s", e)
        return None


def check_flm_version() -> Optional[str]:
    """
    Check if FLM is installed and return version, or None if not available.
    """
    latest_version_str = get_flm_latest_version()
    try:
        result = subprocess.run(
            ["flm", "version"],
            capture_output=True,
            text=True,
            check=True,
            encoding="utf-8",
            errors="replace",
        )

        # Parse version from output like "FLM v0.9.4"
        output = result.stdout.strip()
        if output.startswith("FLM v"):
            version_str = output[5:]  # Remove "FLM v" prefix
            return version_str, latest_version_str
        return None, latest_version_str

    except (subprocess.CalledProcessError, FileNotFoundError):
        return None, latest_version_str


def refresh_environment():
    """
    Refresh PATH to pick up newly installed executables.
    """
    if os.name == "nt":  # Windows
        # On Windows, we need to refresh the PATH from registry
        import winreg

        try:
            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
            ) as key:
                path_value, _ = winreg.QueryValueEx(key, "PATH")
                os.environ["PATH"] = path_value + ";" + os.environ.get("PATH", "")
        except Exception as e:  # pylint: disable=broad-except
            logging.debug("Could not refresh PATH from registry: %s", e)

        # Also try to add common installation paths
        common_paths = [
            r"C:\Program Files\FLM",
            r"C:\Program Files (x86)\FLM",
            os.path.expanduser(r"~\AppData\Local\FLM"),
        ]
        for path in common_paths:
            if os.path.exists(path) and path not in os.environ.get("PATH", ""):
                os.environ["PATH"] = path + ";" + os.environ.get("PATH", "")


def install_flm():
    """
    Check if FLM is installed and at minimum version.
    If not, download and run the GUI installer, then wait for completion.
    """
    # Check current FLM installation
    current_version, latest_version = check_flm_version()

    if (
        current_version
        and latest_version
        and Version(current_version) == Version(latest_version)
    ):
        logging.info(
            "FLM v%s is already installed and is up to date (latest version: v%s).",
            current_version,
            latest_version,
        )
        return

    if current_version:
        if not latest_version:
            logging.info(
                "Unable to detect the latest FLM version; continuing with installed FLM v%s.",
                current_version,
            )
            return
        logging.info(
            "FLM v%s is installed but below latest version v%s. Upgrading...",
            current_version,
            latest_version,
        )
        verysilent = True
    else:
        logging.info("FLM not found. Installing FLM v%s or later...", latest_version)
        verysilent = False

    # Download the installer
    # pylint: disable=line-too-long
    installer_url = "https://github.com/FastFlowLM/FastFlowLM/releases/latest/download/flm-setup.exe"
    installer_path = os.path.join(tempfile.gettempdir(), "flm-setup.exe")
    installer_args = [installer_path, "/VERYSILENT"] if verysilent else [installer_path]

    try:
        # Remove existing installer if present
        if os.path.exists(installer_path):
            os.remove(installer_path)

        logging.info("Downloading FLM installer...")
        response = requests.get(installer_url, stream=True, timeout=30)
        response.raise_for_status()

        # Save installer to disk
        with open(installer_path, "wb") as f:
            for chunk in response.iter_content(chunk_size=8192):
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())

        logging.info("Downloaded FLM installer to %s", installer_path)

        # Launch the installer GUI
        logging.warning(
            "Launching FLM installer GUI. Please complete the installation..."
            if not verysilent
            else "Installing FLM..."
        )

        # Launch installer and wait for it to complete
        if os.name == "nt":  # Windows
            process = subprocess.Popen(installer_args, shell=True)
        else:
            process = subprocess.Popen(installer_args)

        # Wait for installer to complete
        process.wait()

        if process.returncode != 0:
            raise RuntimeError(
                f"FLM installer failed with exit code {process.returncode}"
            )

        logging.info("FLM installer completed successfully")

        # Refresh environment to pick up new PATH entries
        refresh_environment()

        # Wait a moment for system to update
        time.sleep(2)

        # Verify installation
        max_retries = 10
        for attempt in range(max_retries):
            new_version, latest_version = check_flm_version()
            if new_version and Version(new_version) == Version(latest_version):
                logging.info("FLM v%s successfully installed and verified", new_version)
                return

            if attempt < max_retries - 1:
                logging.debug(
                    "FLM not yet available in PATH, retrying... (attempt %d/%d)",
                    attempt + 1,
                    max_retries,
                )
                time.sleep(3)
                refresh_environment()

        # Final check failed
        raise RuntimeError(
            "FLM installation completed but 'flm' command is not available in PATH. "
            "Please ensure FLM is properly installed and available in your system PATH."
        )

    except requests.RequestException as e:
        raise RuntimeError(f"Failed to download FLM installer: {e}") from e
    except Exception as e:
        raise RuntimeError(f"FLM installation failed: {e}") from e
    finally:
        # Clean up installer file
        if os.path.exists(installer_path):
            try:
                os.remove(installer_path)
            except OSError:
                pass  # Ignore cleanup errors


def download_flm_model(config_checkpoint, _=None, do_not_upgrade=False) -> dict:
    """
    Downloads the FLM model for the given configuration.

    Args:
        config_checkpoint: name of the FLM model to install.
        _: placeholder for `config_mmproj`, which is standard
            for WrappedServer (see llamacpp/utils.py) .
        do_not_upgrade: whether to re-download the model if it is already
            available.
    """

    if do_not_upgrade:
        command = ["flm", "pull", f"{config_checkpoint}"]
    else:
        command = ["flm", "pull", f"{config_checkpoint}", "--force"]

    subprocess.run(command, check=True)


def get_flm_installed_models() -> List[str]:
    """
    Parse FLM model list and return installed model checkpoints.

    Returns:
        List of installed FLM model checkpoints (e.g., ["llama3.2:1b", "gemma3:4b"])
    """
    try:
        result = subprocess.run(
            ["flm", "list"],
            capture_output=True,
            text=True,
            check=True,
            encoding="utf-8",
            errors="replace",
        )

        # Check if we got valid output
        if not result.stdout:
            return []

        installed_checkpoints = []

        lines = result.stdout.strip().split("\n")
        for line in lines:
            line = line.strip()
            if line.startswith("- "):
                # Remove the leading "- " and parse the model info
                model_info = line[2:].strip()

                # Check if model is installed (✅)
                if model_info.endswith(" ✅"):
                    checkpoint = model_info[:-2].strip()
                    installed_checkpoints.append(checkpoint)

        return installed_checkpoints

    except (
        subprocess.CalledProcessError,
        FileNotFoundError,
        AttributeError,
        NotADirectoryError,
    ):
        # FLM not installed, not available, or output parsing failed
        return []


def is_flm_available() -> bool:
    """
    Check if FLM is available and meets minimum version requirements.
    """
    current_version, latest_version = check_flm_version()
    return current_version is not None and Version(current_version) == Version(
        latest_version
    )
