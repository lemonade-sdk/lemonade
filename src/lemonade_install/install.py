"""
Utility that helps users install software. It is structured like a
ManagementTool, however it is not a ManagementTool because it cannot
import any lemonade or turnkey modules in order to avoid any installation
collisions on imported modules.
"""

import argparse
import os
import subprocess
import sys
import shutil
import pkg_resources
from pathlib import Path
from typing import Optional
import glob
import zipfile
import requests
import huggingface_hub

DEFAULT_RYZEN_AI_VERSION = "1.4.0"

lemonade_install_dir = Path(__file__).parent.parent.parent
DEFAULT_AMD_OGA_NPU_DIR = os.path.join(
    lemonade_install_dir, "install", "ryzen_ai", "npu"
)
DEFAULT_AMD_OGA_HYBRID_DIR = os.path.join(
    lemonade_install_dir, "install", "ryzen_ai", "hybrid"
)
DEFAULT_QUARK_VERSION = "quark-0.6.0"
DEFAULT_QUARK_DIR = os.path.join(
    lemonade_install_dir, "install", "quark", DEFAULT_QUARK_VERSION
)

npu_install_data = {
    "1.3.0": {
        "artifacts_zipfile": "ryzen_ai_13_ga/npu-llm-artifacts_1.3.0.zip",
        "license_file": (
            "https://account.amd.com/content/dam/account/en/licenses/download/"
            "amd-end-user-license-agreement.pdf"
        ),
        "license_tag": "Beta ",
    },
    "1.4.0": {
        "artifacts_zipfile": "ryzen_ai_14_4571/npu-llm-artifacts_1.4.0.zip",
        "license_file": (
            "https://account.amd.com/content/dam/account/en/licenses/download/"
            "amd-end-user-license-agreement.pdf"
        ),
        "license_tag": "Beta ",
    },
}

hybrid_install_data = {
    "1.3.0": {
        "artifacts_parents_dir": os.path.join(
            DEFAULT_AMD_OGA_HYBRID_DIR, "hybrid-llm-artifacts_1.3.0_lounge"
        ),
        "artifacts_zipfile": (
            "https://www.xilinx.com/bin/public/openDownload?"
            "filename=hybrid-llm-artifacts_1.3.0_012725.zip"
        ),
        "license_file": (
            "https://www.xilinx.com/bin/public/openDownload?"
            "filename=AMD%20End%20User%20License%20Agreement.pdf"
        ),
        "license_tag": "",
    },
    "1.4.0": {
        "artifacts_parents_dir": os.path.join(
            DEFAULT_AMD_OGA_HYBRID_DIR, "hybrid-llm-artifacts_1.4.0"
        ),
        "artifacts_zipfile": (
            "https://www.xilinx.com/bin/public/openDownload?"
            "filename=hybrid-llm-artifacts_1.4.0_032825.zip"
        ),
        "license_file": (
            "https://account.amd.com/content/dam/account/en/licenses/download/"
            "amd-end-user-license-agreement.pdf"
        ),
        "license_tag": "",
    },
}


def get_oga_npu_dir():
    return DEFAULT_AMD_OGA_NPU_DIR


def get_oga_hybrid_artifacts_parent_dir():
    candidates = [f.path for f in os.scandir(DEFAULT_AMD_OGA_HYBRID_DIR) if f.is_dir()]
    if not len(candidates) == 1:
        raise Exception(
            f"Expecting exactly one set of hybrid artifacts at {DEFAULT_AMD_OGA_HYBRID_DIR}"
        )
    return candidates[0]


class ModelManager:

    @property
    def supported_cpu_models(self) -> dict:
        """
        Returns a dictionary of supported CPU models.
        Note: Models must be downloaded before they are locally available.
        """
        return {
            "Qwen2.5-0.5B-Instruct-CPU": {
                "checkpoint": "Qwen/Qwen2.5-0.5B-Instruct",
                "recipe": "hf-cpu",
                "reasoning": False,
            }
        }

    @property
    def supported_hybrid_models(self) -> dict:
        """
        Returns a dictionary of supported hybrid models.
        Note: Models must be downloaded before they are locally available.
        """
        return {
            "Llama-3.2-1B-Instruct-Hybrid": {
                "checkpoint": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "recipe": "oga-hybrid",
                "reasoning": False,
                "max_length": 3000,
            },
            "Llama-3.2-3B-Instruct-Hybrid": {
                "checkpoint": "amd/Llama-3.2-3B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "recipe": "oga-hybrid",
                "reasoning": False,
                "max_length": 2000,
            },
            "Phi-3-Mini-Instruct-Hybrid": {
                "checkpoint": "amd/Phi-3-mini-4k-instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
                "recipe": "oga-hybrid",
                "reasoning": False,
                "max_length": 2000,
            },
            "Qwen-1.5-7B-Chat-Hybrid": {
                "checkpoint": "amd/Qwen1.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid",
                "recipe": "oga-hybrid",
                "reasoning": False,
                "max_length": 3000,
            },
            "DeepSeek-R1-Distill-Llama-8B-Hybrid": {
                "checkpoint": "amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-hybrid",
                "recipe": "oga-hybrid",
                "reasoning": True,
                "max_length": 2000,
            },
            "DeepSeek-R1-Distill-Qwen-7B-Hybrid": {
                "checkpoint": "amd/DeepSeek-R1-Distill-Qwen-7B-awq-asym-uint4-g128-lmhead-onnx-hybrid",
                "recipe": "oga-hybrid",
                "reasoning": True,
                "max_length": 2000,
            },
        }

    @property
    def supported_models(self) -> dict:
        """
        Returns a dictionary of all supported models across all supported backends.
        """
        return {**self.supported_cpu_models, **self.supported_hybrid_models}

    @property
    def downloaded_hf_checkpoints(self) -> list[str]:
        """
        Returns a list of Hugging Face checkpoints that have been downloaded.
        """
        downloaded_hf_checkpoints = []
        try:
            hf_cache_info = huggingface_hub.scan_cache_dir()
            downloaded_hf_checkpoints = [entry.repo_id for entry in hf_cache_info.repos]
        except huggingface_hub.CacheNotFound:
            pass
        except Exception as e:  # pylint: disable=broad-exception-caught
            print(f"Error scanning Hugging Face cache: {e}")
        return downloaded_hf_checkpoints

    @property
    def downloaded_cpu_models(self) -> dict:
        """
        Returns a dictionary of locally available CPU models.
        """
        downloaded_cpu_models = {}
        for model in self.supported_cpu_models:
            if (
                self.supported_cpu_models[model]["checkpoint"]
                in self.downloaded_hf_checkpoints
            ):
                downloaded_cpu_models[model] = self.supported_cpu_models[model]
        return downloaded_cpu_models

    @property
    def downloaded_hybrid_models(self) -> dict:
        """
        Returns a dictionary of locally available hybrid models.
        """
        downloaded_hybrid_models = {}
        for model in self.supported_hybrid_models:
            if (
                self.supported_hybrid_models[model]["checkpoint"]
                in self.downloaded_hf_checkpoints
            ):
                downloaded_hybrid_models[model] = self.supported_hybrid_models[model]
        return downloaded_hybrid_models

    @property
    def downloaded_models_enabled(self) -> dict:
        """
        Returns a dictionary of locally available models that are enabled by the current installation.
        """
        downloaded_models_enabled = self.downloaded_cpu_models.copy()
        if (
            "onnxruntime-vitisai" in pkg_resources.working_set.by_key
            and "onnxruntime-genai-directml-ryzenai" in pkg_resources.working_set.by_key
        ):
            downloaded_models_enabled.update(self.downloaded_hybrid_models)
        return downloaded_models_enabled

    def download_models(self, models: list[str]):
        """
        Downloads the specified models from Hugging Face.
        """
        for model in models:
            if model not in self.supported_models:
                raise ValueError(
                    f"Model {model} is not supported. Please choose from the following: {list(self.supported_models.keys())}"
                )
            checkpoint = self.supported_models[model]["checkpoint"]
            print(f"Downloading {model} ({checkpoint})")
            huggingface_hub.snapshot_download(repo_id=checkpoint)


def download_lfs_file(token, file, output_filename):
    """Downloads a file from LFS"""
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
            choices=["npu", "hybrid", "npu-1.3.0", "hybrid-1.3.0"],
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
            "--models",
            help="One or more models to download",
            type=str,
            nargs="+",
            choices=ModelManager().supported_models,
        )

        return parser

    def run(
        self,
        ryzenai: Optional[str] = None,
        quark: Optional[str] = None,
        yes: bool = False,
        token: Optional[str] = None,
        models: Optional[str] = None,
    ):
        if ryzenai is None and quark is None and models is None:
            raise ValueError(
                "You must select something to install, for example `--ryzenai`, `--quark`, or `--models`"
            )

        # Download models if needed
        if models is not None:
            model_manager = ModelManager()
            model_manager.download_models(models)

        if ryzenai is not None:
            version = DEFAULT_RYZEN_AI_VERSION
            if ryzenai == "npu-1.3.0":
                ryzenai = "npu"
                version = "1.3.0"
            if ryzenai == "hybrid-1.3.0":
                ryzenai = "hybrid"
                version = "1.3.0"
            if ryzenai == "npu":
                # Check version is valid
                if version not in npu_install_data:
                    raise ValueError(
                        "Invalid version for NPU.  Valid options are "
                        f"{list(npu_install_data.keys())}."
                    )
                file = npu_install_data[version].get("artifacts_zipfile", None)
                install_dir = DEFAULT_AMD_OGA_NPU_DIR
                wheels_full_path = os.path.join(install_dir, "amd_oga/wheels")
                license_file = npu_install_data[version].get("license_file", None)
                license_tag = npu_install_data[version].get("license_tag", None)
            elif ryzenai == "hybrid":
                if version not in hybrid_install_data:
                    raise ValueError(
                        "Invalid version for Hybrid.  Valid options are "
                        f"{hybrid_install_data.keys()}."
                    )
                file = hybrid_install_data[version].get("artifacts_zipfile", None)
                install_dir = DEFAULT_AMD_OGA_HYBRID_DIR
                wheels_full_path = os.path.join(
                    hybrid_install_data[version]["artifacts_parents_dir"],
                    "hybrid-llm-artifacts",
                    "onnxruntime_genai",
                    "wheel",
                )
                license_file = hybrid_install_data[version].get("license_file", None)
                license_tag = hybrid_install_data[version].get("license_tag", None)
            else:
                raise ValueError(
                    f"Value passed to ryzenai argument is not supported: {ryzenai}"
                )

            if license_file:
                if yes:
                    print(
                        f"\nYou have accepted the AMD {license_tag}Software End User License "
                        f"Agreement for Ryzen AI {ryzenai} by providing the `--yes` option. "
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
                        raise LicenseRejected(
                            "Exiting because the license was not accepted."
                        )

            print(f"Downloading {ryzenai} artifacts for Ryzen AI {version}.")
            print("Wheels will be added to current activated environment.")

            archive_file_name = f"oga_{ryzenai}.zip"
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

            # Write artifacts filename to text file
            with open(
                os.path.join(install_dir, f"{version}.txt"), "w", encoding="utf-8"
            ) as f:
                f.write(f"Installed artifacts: {file}")

            # Install all whl files in the specified wheels folder
            print(f"\nInstalling wheels from {wheels_full_path}\n")
            if version == "1.3.0":
                # Install one wheel file at a time (1.3.0 npu build only works this way)
                for file in os.listdir(wheels_full_path):
                    if file.endswith(".whl"):
                        install_cmd = (
                            f"{sys.executable} -m pip install "
                            f"{os.path.join(wheels_full_path, file)}"
                        )
                        print(f"\nInstalling {file} with command {install_cmd}\n")
                        subprocess.run(install_cmd, check=True, shell=True)
            else:
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

        if quark is not None:
            quark_install_dir = os.path.join(lemonade_install_dir, "install", "quark")
            os.makedirs(quark_install_dir, exist_ok=True)

            # Install Quark utilities
            quark_url = f"https://www.xilinx.com/bin/public/openDownload?filename=quark-{quark}.zip"
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
            wheel_path = os.path.join(
                quark_install_dir, f"quark-{quark}-py3-none-any.whl"
            )
            print(f"\nInstalling Quark wheel from {wheel_url}")
            download_file(wheel_url, wheel_path, "wheel file")

            install_cmd = f"{sys.executable} -m pip install --no-deps {wheel_path}"
            subprocess.run(install_cmd, check=True, shell=True)
            os.remove(wheel_path)

            print(f"\nQuark installed successfully at: {quark_path}")


def main():
    installer = Install()
    args = installer.parser().parse_args()
    installer.run(**args.__dict__)


if __name__ == "__main__":
    main()
