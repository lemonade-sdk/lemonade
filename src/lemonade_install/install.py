# Utility that helps users install software. It is structured like a
# ManagementTool, however it is not a ManagementTool because it cannot
# import any lemonade modules in order to avoid any installation
# collisions on imported modules.
#
# This tool can install Ryzen AI software artifacts (libraries, wheels, etc.).
# The Ryzen AI hybrid artifacts directory hierarchy is:
#
#     RYZEN_AI\hybrid
#         libs
#         wheels
#
# The Ryzen AI npu artifacts directory hierarchy is:
#
#     RYZEN_AI\npu
#         libs
#         wheels
#
# In the above, RYZEN_AI is the path to the folder ryzen_ai that is contained in the
# same folder as the executable python.exe.
# The folder RYZEN_AI also contains a file `version_info.json` that contains information
# about the installed Ryzen AI artifacts.
#
# In any python environment, only one set of artifacts can be installed at a time.
# Python environments created by Lemonade v6.1.x or earlier will need to be recreated.
#

import argparse
import glob
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Optional
import zipfile

DEFAULT_RYZEN_AI_VERSION = "1.4.0"
version_info_filename = "version_info.json"

# NPU Driver configuration
NPU_DRIVER_DOWNLOAD_URL = (
    "https://account.amd.com/en/forms/downloads/"
    "ryzenai-eula-public-xef.html?filename=NPU_RAI1.5_280_WHQL.zip"
)
REQUIRED_NPU_DRIVER_VERSION = "32.0.203.280"

lemonade_install_dir = Path(__file__).parent.parent.parent
DEFAULT_QUARK_VERSION = "quark-0.6.0"
DEFAULT_QUARK_DIR = os.path.join(
    lemonade_install_dir, "install", "quark", DEFAULT_QUARK_VERSION
)

# List of supported Ryzen AI processor series (can be extended in the future)
SUPPORTED_RYZEN_AI_SERIES = ["300"]

npu_install_data = {
    "1.4.0": {
        "artifacts_zipfile": (
            "https://www.xilinx.com/bin/public/openDownload?"
            "filename=npu-llm-artifacts_1.4.0_032925.zip"
        ),
        "license_file": (
            "https://account.amd.com/content/dam/account/en/licenses/download/"
            "amd-end-user-license-agreement.pdf"
        ),
        "license_tag": "",
    },
}

hybrid_install_data = {
    "1.4.0": {
        "artifacts_zipfile": (
            "https://www.xilinx.com/bin/public/openDownload?"
            "filename=hybrid-llm-artifacts_1.4.0_032925.zip"
        ),
        "license_file": (
            "https://account.amd.com/content/dam/account/en/licenses/download/"
            "amd-end-user-license-agreement.pdf"
        ),
        "license_tag": "",
    },
}

model_prep_install_data = {
    "1.4.0": {
        "model_prep_artifacts_zipfile": (
            "https://www.xilinx.com/bin/public/openDownload?"
            "filename=model-prep-artifacts-1.4.0-0331.zip"
        )
    }
}


def get_ryzen_ai_path(check_exists=True):
    python_executable_path = sys.executable
    python_executable_dir = os.path.dirname(python_executable_path)
    ryzen_ai_folder = os.path.join(python_executable_dir, "ryzen_ai")
    if check_exists:
        if not os.path.isdir(ryzen_ai_folder):
            raise RuntimeError(
                "Please use `lemonade-install --ryzenai` to install Ryzen AI into the current "
                "python environment. For more info, see "
                "https://ryzenai.docs.amd.com/en/latest/llm/high_level_python.html."
            )
    return ryzen_ai_folder


def get_ryzen_ai_version_info():
    ryzen_ai_folder = get_ryzen_ai_path()
    version_info_path = os.path.join(ryzen_ai_folder, version_info_filename)
    if not os.path.isfile(version_info_path):
        raise RuntimeError(
            f"The file {version_info_filename} is missing from the Ryzen AI "
            f"folder {ryzen_ai_folder}.  Please run `lemonade-install` to fix.  For more info, see "
            "https://ryzenai.docs.amd.com/en/latest/llm/high_level_python.html."
        )
    with open(version_info_path, encoding="utf-8") as file:
        version_info = json.load(file)
    return version_info


def get_oga_npu_dir():
    version_info = get_ryzen_ai_version_info()
    version = version_info["version"]
    ryzen_ai_folder = get_ryzen_ai_path()
    npu_dir = os.path.join(ryzen_ai_folder, "npu")
    if not os.path.isdir(npu_dir):
        raise RuntimeError(
            f"The npu artifacts are missing from the Ryzen AI folder {ryzen_ai_folder}. "
            " Please run `lemonade-install --ryzenai npu` again to fix.  For more info, see"
            "https://ryzenai.docs.amd.com/en/latest/llm/high_level_python.html."
        )
    return npu_dir, version


def get_oga_hybrid_dir():
    version_info = get_ryzen_ai_version_info()
    version = version_info["version"]
    ryzen_ai_folder = get_ryzen_ai_path()
    hybrid_dir = os.path.join(ryzen_ai_folder, "hybrid")
    if not os.path.isdir(hybrid_dir):
        raise RuntimeError(
            f"The hybrid artifacts are missing from the Ryzen AI folder {ryzen_ai_folder}. "
            " Please run `lemonade-install --ryzenai hybrid` again to fix.  For more info, see "
            "https://ryzenai.docs.amd.com/en/latest/llm/high_level_python.html."
        )
    return hybrid_dir, version


def _get_ryzenai_version_info(device=None):
    """
    Centralized version detection for RyzenAI installations.
    Uses lazy imports to avoid import errors when OGA is not installed.
    """
    try:
        # Lazy import to avoid errors when OGA is not installed
        from packaging.version import Version
        import onnxruntime_genai as og

        if Version(og.__version__) >= Version("0.7.0"):
            oga_path = os.path.dirname(og.__file__)
            if og.__version__ in ("0.7.0.2.1", "0.7.0.2"):
                return "1.5.0", oga_path
            else:
                return "1.4.0", oga_path
        else:
            if device == "npu":
                oga_path, version = get_oga_npu_dir()
            else:
                oga_path, version = get_oga_hybrid_dir()
            return version, oga_path
    except ImportError as e:
        raise ImportError(
            f"{e}\n Please install lemonade-sdk with "
            "one of the oga extras, for example:\n"
            "pip install lemonade-sdk[dev,oga-cpu]\n"
            "See https://lemonade_server.ai/install_options.html for details"
        ) from e


def download_lfs_file(token, file, output_filename):
    """Downloads a file from LFS"""
    import requests

    # Set up the headers for the request
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json",
    }

    response = requests.get(
        f"https://api.github.com/repos/aigdat/ryzenai-sw-ea/contents/{file}",
        headers=headers,
    )

    # Check if the request was successful
    if response.status_code == 200:
        # Parse the JSON response to get the download URL
        content = response.json()
        download_url = content.get("download_url")

        if download_url:
            # Download the file from the download URL
            file_response = requests.get(download_url)

            # Write the content to a file
            with open(output_filename, "wb") as file:
                file.write(file_response.content)
        else:
            print("Download URL not found in the response.")
    else:
        raise ValueError(
            "Failed to fetch the content from GitHub API. "
            f"Status code: {response.status_code}, Response: {response.json()}"
        )

    if not os.path.isfile(output_filename):
        raise ValueError(f"Error: {output_filename} does not exist.")


def download_file(url: str, output_filename: str, description: str = None):
    import requests

    try:
        response = requests.get(url)
        if response.status_code != 200:
            raise Exception(
                f"Failed to fetch the content from GitHub API. \
                Status code: {response.status_code}, Response: {response.json()}"
            )

        with open(output_filename, "wb") as file:
            file.write(response.content)

        if not os.path.isfile(output_filename):
            raise Exception(f"\nError: Failed to write to {output_filename}")

    except Exception as e:
        raise Exception(f"\nError downloading {description or 'file'}: {str(e)}")


def unzip_file(zip_path, extract_to):
    """Unzips the specified zip file to the given directory."""
    with zipfile.ZipFile(zip_path, "r") as zip_ref:
        zip_ref.extractall(extract_to)


def check_ryzen_ai_processor():
    """
    Checks if the current system has a supported Ryzen AI processor.

    Raises:
        UnsupportedPlatformError: If the processor is not a supported Ryzen AI models.
    """
    if not sys.platform.startswith("win"):
        raise UnsupportedPlatformError(
            "Ryzen AI installation is only supported on Windows."
        )

    skip_check = os.getenv("RYZENAI_SKIP_PROCESSOR_CHECK", "").lower() in {
        "1",
        "true",
        "yes",
    }
    if skip_check:
        print("[WARNING]: Processor check skipped.")
        return

    is_supported = False
    cpu_name = ""

    try:
        # Use PowerShell command to get processor name
        powershell_cmd = [
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            "Get-WmiObject -Class Win32_Processor | Select-Object -ExpandProperty Name",
        ]

        result = subprocess.run(
            powershell_cmd,
            capture_output=True,
            text=True,
            check=True,
        )

        # Extract the CPU name from PowerShell output
        cpu_name = result.stdout.strip()
        if not cpu_name:
            print(
                "[WARNING]: Could not detect processor name. Proceeding with installation."
            )
            return

        # Check for any supported series
        for series in SUPPORTED_RYZEN_AI_SERIES:
            # Look for the series number pattern - matches any processor in the supported series
            pattern = rf"ryzen ai.*\b{series[0]}\d{{2}}\b"
            match = re.search(pattern, cpu_name.lower(), re.IGNORECASE)

            if match:
                is_supported = True
                break

        if not is_supported:
            print(
                f"[WARNING]: Processor '{cpu_name}' may not be officially supported for Ryzen AI hybrid execution."
            )
            print(
                "[WARNING]: Installation will proceed, but hybrid features may not work correctly."
            )
            print("[WARNING]: Officially supported processors: Ryzen AI 300-series")

    except Exception as e:  # pylint: disable=broad-exception-caught
        print(
            f"[WARNING]: Could not detect processor ({e}). Proceeding with installation."
        )
        print("[WARNING]: Hybrid features may not work if processor is not supported.")


def download_and_extract_package(
    url: str,
    version: str,
    install_dir: str,
    package_name: str,
) -> str:
    """
    Downloads, Extracts and Renames the folder

    Args:
        url: Download URL for the package
        version: Version string
        install_dir: Directory to install to
        package_name: Name of the package

    Returns:
        str: Path where package was extracted (renamed to package-version)
    """
    import requests

    zip_filename = f"{package_name}-{version}.zip"
    zip_path = os.path.join(install_dir, zip_filename)
    target_folder = os.path.join(install_dir, f"{package_name}-{version}")

    print(f"\nDownloading {package_name} from {url}")
    response = requests.get(url)
    if response.status_code == 200:
        with open(zip_path, "wb") as f:
            f.write(response.content)
    else:
        raise Exception(
            f"Failed to download {package_name}. Status code: {response.status_code}"
        )

    print("\n[INFO]: Extracting zip file ...")
    with zipfile.ZipFile(zip_path, "r") as zip_ref:
        zip_ref.extractall(install_dir)
    print("\n[INFO]: Extraction completed.")

    os.remove(zip_path)

    extracted_folder = None
    for folder in os.listdir(install_dir):
        folder_path = os.path.join(install_dir, folder)
        if os.path.isdir(folder_path) and folder.startswith(f"{package_name}-"):
            extracted_folder = folder_path
            break

    if extracted_folder is None:
        raise ValueError(
            f"Error: Extracted folder for {package_name} version {version} not found."
        )

    # Rename extracted folder to package-version
    if extracted_folder != target_folder:
        if os.path.exists(target_folder):
            shutil.rmtree(target_folder)  # Remove if already exists
        os.rename(extracted_folder, target_folder)
        print(f"\n[INFO]: Renamed folder to {target_folder}")

    return target_folder


class LicenseRejected(Exception):
    """
    Raise an exception if the user rejects the license prompt.
    """


class UnsupportedPlatformError(Exception):
    """
    Raise an exception if the hardware is not supported.
    """


class Install:
    """
    Installs the necessary software for specific lemonade features.
    """

    @staticmethod
    def parser() -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser(
            description="Installs the necessary software for specific lemonade features",
        )

        parser.add_argument(
            "--ryzenai",
            help="Install Ryzen AI software for LLMs. Requires an authentication token. "
            "The 'npu' and 'hybrid' choices install the default "
            f"{DEFAULT_RYZEN_AI_VERSION} version.",
            choices=[
                "npu",
                "hybrid",
                "unified",
                "npu-1.4.0",
                "hybrid-1.4.0",
                "unified-1.4.0",
            ],
        )

        parser.add_argument(
            "--build-model",
            action="store_true",
            help="If specified, installs additional model prep artifacts for Ryzen AI.",
        )

        parser.add_argument(
            "-y",
            "--yes",
            action="store_true",
            help="Answer 'yes' to all questions. "
            "Make sure to review all legal agreements before selecting this option.",
        )

        parser.add_argument(
            "--token",
            help="Some software requires an authentication token to download. "
            "If this argument is not provided, the token can come from an environment "
            "variable (e.g., Ryzen AI uses environment variable OGA_TOKEN).",
        )

        parser.add_argument(
            "--quark",
            help="Install Quark Quantization tool for LLMs",
            choices=["0.6.0"],
        )

        parser.add_argument(
            "--llamacpp",
            help="Install llama.cpp binaries with specified backend",
            choices=["rocm", "vulkan"],
        )

        return parser

    @staticmethod
    def _get_license_acceptance(version, license_file, license_tag, yes):
        if yes:
            print(
                f"\nYou have accepted the AMD {license_tag}Software End User License "
                f"Agreement for Ryzen AI {version} by providing the `--yes` option. "
                "The license file is available for your review at "
                f"{license_file}\n"
            )
        else:
            print(
                f"\nYou must accept the AMD {license_tag}Software End User License "
                "Agreement in order to install this software. To continue, type the word "
                "yes to assert that you agree and are authorized to agree "
                "on behalf of your organization, to the terms and "
                f"conditions, in the {license_tag}Software End User License Agreement, "
                "which terms and conditions may be reviewed, downloaded and "
                "printed from this link: "
                f"{license_file}\n"
            )

            response = input("Would you like to accept the license (yes/No)? ")
            if response.lower() == "yes" or response.lower() == "y":
                pass
            else:
                raise LicenseRejected("Exiting because the license was not accepted.")

    @staticmethod
    def _install_artifacts(version, install_dir, token, file, wheels_full_path):
        archive_file_name = "artifacts.zip"
        archive_file_path = os.path.join(install_dir, archive_file_name)

        if token:
            token_to_use = token
        else:
            token_to_use = os.environ.get("OGA_TOKEN")

        # Retrieve the installation artifacts
        if os.path.exists(install_dir):
            # Remove any artifacts from a previous installation attempt
            shutil.rmtree(install_dir)
        os.makedirs(install_dir)
        if any(proto in file for proto in ["https:", "http:"]):
            print(f"\nDownloading {file}\n")
            download_file(file, archive_file_path)
        elif "file:" in file:
            local_file = file.replace("file://", "C:/")
            print(f"\nCopying {local_file}\n")
            shutil.copy(local_file, archive_file_path)
        else:
            print(f"\nDownloading {file} from GitHub LFS to {install_dir}\n")
            download_lfs_file(token_to_use, file, archive_file_path)

        # Unzip the file
        print(f"\nUnzipping archive {archive_file_path}\n")
        unzip_file(archive_file_path, install_dir)
        print(f"\nDLLs installed\n")

        # Install all whl files in the specified wheels folder
        if wheels_full_path is not None:
            print(f"\nInstalling wheels from {wheels_full_path}\n")
            # Install all the wheel files together, allowing pip to work out the dependencies
            wheel_files = glob.glob(os.path.join(wheels_full_path, "*.whl"))
            install_cmd = [sys.executable, "-m", "pip", "install"] + wheel_files
            subprocess.run(
                install_cmd,
                check=True,
                shell=True,
            )

        # Delete the zip file
        print(f"\nCleaning up, removing {archive_file_path}\n")
        os.remove(archive_file_path)

    @staticmethod
    def _install_ryzenai_model_artifacts(ryzen_ai_folder, version):
        """
        Installs the Ryzen AI model prep artifacts if the build-model flag is provided.

        Args:
            ryzen_ai_folder (str): Path to the Ryzen AI installation folder.
            version (str): Version of the Ryzen AI artifacts.
        """

        # Check if model prep artifacts are available for the given version
        if version not in model_prep_install_data:
            raise ValueError(
                "Model prep artifacts are only available "
                "for version 1.4.0 and above, "
                "and only for 'hybrid' and 'npu-only' targets."
            )

        # Get the model prep artifacts zipfile URL
        file = model_prep_install_data[version].get(
            "model_prep_artifacts_zipfile", None
        )
        if not file:
            raise ValueError(
                "Model prep artifacts zipfile URL is missing for version " f"{version}."
            )

        # Download and extract the model prep artifacts
        print(f"\nDownloading model prep artifacts for Ryzen AI {version}.")
        model_prep_dir = download_and_extract_package(
            url=file,
            version=version,
            install_dir=ryzen_ai_folder,
            package_name="model-prep",
        )

        # Install all .whl files from the extracted model prep artifacts
        wheels_full_path = os.path.join(model_prep_dir)
        print(f"\nInstalling model prep wheels from {wheels_full_path}\n")
        wheel_files = glob.glob(os.path.join(wheels_full_path, "*.whl"))
        if not wheel_files:
            raise ValueError(
                f"No .whl files found in the model prep artifacts directory: {wheels_full_path}"
            )

        install_cmd = [sys.executable, "-m", "pip", "install"] + wheel_files
        subprocess.run(install_cmd, check=True, shell=True)

        print(f"\nModel prep artifacts installed successfully from {model_prep_dir}.")
        return file

    @staticmethod
    def _install_ryzenai_npu(ryzen_ai_folder, version, yes, token, skip_wheels=False):
        # Check version is valid
        if version not in npu_install_data:
            raise ValueError(
                "Invalid Ryzen AI version for NPU.  Valid options are "
                f"{list(npu_install_data.keys())}."
            )
        file = npu_install_data[version].get("artifacts_zipfile", None)
        license_file = npu_install_data[version].get("license_file", None)
        license_tag = npu_install_data[version].get("license_tag", None)
        install_dir = os.path.join(ryzen_ai_folder, "npu")
        wheels_full_path = os.path.join(install_dir, "wheels")

        if license_file:
            Install._get_license_acceptance(version, license_file, license_tag, yes)

        print(f"Downloading NPU artifacts for Ryzen AI {version}.")
        if skip_wheels:
            wheels_full_path = None
        else:
            print("Wheels will be added to current activated environment.")
        Install._install_artifacts(version, install_dir, token, file, wheels_full_path)
        return file

    @staticmethod
    def _install_ryzenai_hybrid(
        ryzen_ai_folder, version, yes, token, skip_wheels=False
    ):
        # Check version is valid
        if version not in hybrid_install_data:
            raise ValueError(
                "Invalid version for hybrid.  Valid options are "
                f"{list(hybrid_install_data.keys())}."
            )
        file = hybrid_install_data[version].get("artifacts_zipfile", None)
        license_file = hybrid_install_data[version].get("license_file", None)
        license_tag = hybrid_install_data[version].get("license_tag", None)
        install_dir = os.path.join(ryzen_ai_folder, "hybrid")
        wheels_full_path = os.path.join(install_dir, "wheels")

        if license_file:
            Install._get_license_acceptance(version, license_file, license_tag, yes)

        print(f"Downloading hybrid artifacts for Ryzen AI {version}.")
        if skip_wheels:
            wheels_full_path = None
        else:
            print("Wheels will be added to current activated environment.")
        Install._install_artifacts(version, install_dir, token, file, wheels_full_path)
        return file

    @staticmethod
    def _install_ryzenai(ryzenai, build_model, yes, token):
        # Check if the processor is supported before proceeding
        check_ryzen_ai_processor()

        warning_msg = (
            "\n" + "=" * 80 + "\n"
            "WARNING: IMPORTANT: NEW RYZEN AI 1.5.0 INSTALLATION PROCESS\n"
            + "=" * 80
            + "\n"
            "Starting with Ryzen AI 1.5.0, installation is now available through PyPI.\n"
            "For new installations, consider using:\n\n"
            "pip install lemonade-sdk[oga-ryzenai] --extra-index-url https://pypi.amd.com/simple\n\n"
            "This legacy installation method (lemonade-install --ryzenai) is still\n"
            "supported for version 1.4.0, but may be deprecated in future releases.\n"
            + "=" * 80
            + "\n"
        )
        print(warning_msg)

        # Delete any previous Ryzen AI installation in this environment
        ryzen_ai_folder = get_ryzen_ai_path(check_exists=False)
        if os.path.exists(ryzen_ai_folder):
            print("Deleting previous Ryzen AI installation in this environment.")
            shutil.rmtree(ryzen_ai_folder)

        # Determine Ryzen AI version to install
        version = DEFAULT_RYZEN_AI_VERSION
        if "-" in ryzenai:
            # ryzenai is: (npu|hybrid)-(<VERSION>)
            parts = ryzenai.split("-")
            ryzenai = parts[0]
            version = parts[1]

        # Install artifacts needed for npu, hybrid, or unified (both) inference
        npu_file = None
        hybrid_file = None
        model_prep_file = None
        if ryzenai == "npu":
            npu_file = Install._install_ryzenai_npu(
                ryzen_ai_folder, version, yes, token
            )
        elif ryzenai == "hybrid":
            hybrid_file = Install._install_ryzenai_hybrid(
                ryzen_ai_folder, version, yes, token
            )
        elif ryzenai == "unified":
            # Download NPU artifacts and use the NPU wheels
            npu_file = Install._install_ryzenai_npu(
                ryzen_ai_folder, version, yes, token
            )
            # Download Hybrid artifacts (and skip wheels as they are already loaded)
            hybrid_file = Install._install_ryzenai_hybrid(
                ryzen_ai_folder, version, yes, token, skip_wheels=True
            )
        else:
            raise ValueError(
                f"Value passed to ryzenai argument is not supported: {ryzenai}"
            )

        if build_model:
            model_prep_file = Install._install_ryzenai_model_artifacts(
                ryzen_ai_folder, version
            )

        # Write version_info file
        version_info = {
            "version": version,
            "hybrid_artifacts": hybrid_file,
            "npu_artifacts": npu_file,
            "model_prep_artifacts": model_prep_file,
        }
        version_info_path = os.path.join(ryzen_ai_folder, version_info_filename)
        try:
            with open(version_info_path, "w", encoding="utf-8") as file:
                json.dump(version_info, file, indent=4)
        except IOError as e:
            print(f"An error occurred while writing {version_info_path}: {e}")

    @staticmethod
    def _install_quark(quark):
        quark_install_dir = os.path.join(lemonade_install_dir, "install", "quark")
        os.makedirs(quark_install_dir, exist_ok=True)

        # Install Quark utilities
        quark_url = (
            f"https://www.xilinx.com/bin/public/openDownload?filename=quark-{quark}.zip"
        )
        quark_path = download_and_extract_package(
            url=quark_url,
            version=quark,
            install_dir=quark_install_dir,
            package_name="quark",
        )
        # Install Quark wheel
        wheel_url = (
            "https://www.xilinx.com/bin/public/openDownload?"
            f"filename=quark-{quark}-py3-none-any.whl"
        )
        wheel_path = os.path.join(quark_install_dir, f"quark-{quark}-py3-none-any.whl")
        print(f"\nInstalling Quark wheel from {wheel_url}")
        download_file(wheel_url, wheel_path, "wheel file")

        install_cmd = f"{sys.executable} -m pip install --no-deps {wheel_path}"
        subprocess.run(install_cmd, check=True, shell=True)
        os.remove(wheel_path)

        print(f"\nQuark installed successfully at: {quark_path}")

    @staticmethod
    def _install_llamacpp(backend):
        """
        Install llama.cpp binaries with the specified backend.

        Args:
            backend: The backend to use ('rocm' or 'vulkan')
        """

        from lemonade.tools.llamacpp.utils import install_llamacpp

        install_llamacpp(backend)

    def run(
        self,
        ryzenai: Optional[str] = None,
        build_model: Optional[str] = None,
        quark: Optional[str] = None,
        llamacpp: Optional[str] = None,
        yes: bool = False,
        token: Optional[str] = None,
    ):
        if ryzenai is None and quark is None and llamacpp is None:
            raise ValueError(
                "You must select something to install, "
                "for example `--ryzenai`, `--quark`, or `--llamacpp`"
            )

        if ryzenai is not None:
            self._install_ryzenai(ryzenai, build_model, yes, token)

        if quark is not None:
            self._install_quark(quark)

        if llamacpp is not None:
            self._install_llamacpp(llamacpp)


def main():
    installer = Install()
    args = installer.parser().parse_args()
    installer.run(**args.__dict__)


if __name__ == "__main__":
    main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
