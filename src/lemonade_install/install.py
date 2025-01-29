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
from typing import Optional
import zipfile
import requests
from pathlib import Path


lemonade_install_dir = Path(__file__).parent.parent.parent
DEFAULT_AMD_OGA_NPU_DIR = os.path.join(
    lemonade_install_dir, "install", "ryzen_ai", "npu"
)
DEFAULT_AMD_OGA_HYBRID_DIR = os.path.join(
    lemonade_install_dir, "install", "ryzen_ai", "hybrid"
)
DEFAULT_AMD_OGA_HYBRID_ARTIFACTS_PARENT_DIR = os.path.join(
    DEFAULT_AMD_OGA_HYBRID_DIR,
    "hybrid-llm-artifacts_1.3.0_lounge",
)


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


def download_file(url, output_filename):
    response = requests.get(url)

    with open(output_filename, "wb") as file:
        file.write(response.content)


def unzip_file(zip_path, extract_to):
    """Unzips the specified zip file to the given directory."""
    with zipfile.ZipFile(zip_path, "r") as zip_ref:
        zip_ref.extractall(extract_to)


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
            help="Install Ryzen AI software for LLMs. Requires an authentication token.",
            choices=["npu", "hybrid", None],
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

        return parser

    def run(
        self,
        ryzenai: Optional[str] = None,
        yes: bool = False,
        token: Optional[str] = None,
    ):

        if ryzenai is not None:
            if ryzenai == "npu":
                file = "ryzen_ai_13_ga/npu-llm-artifacts_1.3.0.zip"
                install_dir = DEFAULT_AMD_OGA_NPU_DIR
                wheels_full_path = os.path.join(install_dir, "amd_oga/wheels")
                license = "https://account.amd.com/content/dam/account/en/licenses/download/amd-end-user-license-agreement.pdf"
                license_tag = "Beta "
            elif ryzenai == "hybrid":
                file = "https://www.xilinx.com/bin/public/openDownload?filename=hybrid-llm-artifacts_1.3.0_012725.zip"
                install_dir = DEFAULT_AMD_OGA_HYBRID_DIR
                wheels_full_path = os.path.join(
                    DEFAULT_AMD_OGA_HYBRID_ARTIFACTS_PARENT_DIR,
                    "hybrid-llm-artifacts",
                    "onnxruntime_genai",
                    "wheel",
                )
                license = r"https://www.xilinx.com/bin/public/openDownload?filename=AMD%20End%20User%20License%20Agreement.pdf"
                license_tag = ""
            else:
                raise ValueError(
                    f"Value passed to ryzenai argument is not supported: {ryzenai}"
                )

            if yes:
                print(
                    f"\nYou have accepted the AMD {license_tag}Software End User License Agreement for "
                    f"Ryzen AI {ryzenai} by providing the `--yes` option. "
                    "The license file is available for your review at "
                    # pylint: disable=line-too-long
                    f"{license}\n"
                )
            else:
                print(
                    f"\nYou must accept the AMD {license_tag}Software End User License Agreement in "
                    "order to install this software. To continue, type the word yes "
                    "to assert that you agree and are authorized to agree "
                    "on behalf of your organization, to the terms and "
                    f"conditions, in the {license_tag}Software End User License Agreement, "
                    "which terms and conditions may be reviewed, downloaded and "
                    "printed from this link: "
                    # pylint: disable=line-too-long
                    f"{license}\n"
                )

                response = input("Would you like to accept the license (yes/No)? ")
                if response.lower() == "yes" or response.lower() == "y":
                    pass
                else:
                    raise LicenseRejected(
                        "Exiting because the license was not accepted."
                    )

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
            if ryzenai == "npu":
                print(f"\nDownloading {file} from GitHub LFS to {install_dir}\n")
                download_lfs_file(token_to_use, file, archive_file_path)
            elif ryzenai == "hybrid":
                print(f"\nDownloading {file}\n")
                download_file(file, archive_file_path)

            # Unzip the file
            print(f"\nUnzipping archive {archive_file_path}\n")
            unzip_file(archive_file_path, install_dir)

            # Install all whl files in the specified wheels folder
            print(f"\nInstalling wheels from {wheels_full_path}\n")
            for file in os.listdir(wheels_full_path):
                if file.endswith(".whl"):
                    install_cmd = f"{sys.executable} -m pip install {os.path.join(wheels_full_path, file)}"

                    print(f"\nInstalling {file} with command {install_cmd}\n")

                    subprocess.run(
                        install_cmd,
                        check=True,
                        shell=True,
                    )

            # Delete the zip file
            print(f"\nCleaning up, removing {archive_file_path}\n")
            os.remove(archive_file_path)
        else:
            raise ValueError(
                "You must select something to install, for example `--ryzenai`"
            )


def main():
    installer = Install()
    args = installer.parser().parse_args()
    installer.run(**args.__dict__)


if __name__ == "__main__":
    main()
