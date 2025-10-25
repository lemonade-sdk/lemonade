# Utility that helps users install software. It is structured like a
# ManagementTool, however it is not a ManagementTool because it cannot
# import any lemonade modules in order to avoid any installation
# collisions on imported modules.
#
# This tool can install llama.cpp and FLM (FastFlowLM) for local LLM inference.
# For RyzenAI support, use PyPI installation:
#   pip install lemonade-sdk[oga-ryzenai] --extra-index-url https://pypi.amd.com/simple

import argparse
import os
import re
import subprocess
import sys
from typing import Optional

# NPU Driver configuration
NPU_DRIVER_DOWNLOAD_URL = (
    "https://account.amd.com/en/forms/downloads/"
    "ryzenai-eula-public-xef.html?filename=NPU_RAI1.5_280_WHQL.zip"
)
REQUIRED_NPU_DRIVER_VERSION = "32.0.203.280"

# List of supported Ryzen AI processor series (can be extended in the future)
SUPPORTED_RYZEN_AI_SERIES = ["300"]


def _get_ryzenai_version_info(device=None):
    """
    Centralized version detection for RyzenAI installations.
    Uses lazy imports to avoid import errors when OGA is not installed.
    """
    try:
        # Lazy import to avoid errors when OGA is not installed
        from packaging.version import Version

        # For embedded Python on Windows, add DLL directory before importing onnxruntime_genai
        # This is required to find DirectML.dll and other dependencies
        if sys.platform.startswith("win"):
            import site

            site_packages = site.getsitepackages()
            for sp in site_packages:
                oga_dir = os.path.join(sp, "onnxruntime_genai")
                if os.path.exists(oga_dir):
                    # Add the onnxruntime_genai directory to DLL search path
                    # This ensures DirectML.dll and onnxruntime.dll can be found
                    os.add_dll_directory(oga_dir)
                    break

        import onnxruntime_genai as og

        if Version(og.__version__) >= Version("0.7.0"):
            oga_path = os.path.dirname(og.__file__)
            if og.__version__ in ("0.9.2",):
                return "1.6.0", oga_path
            else:
                raise ValueError(
                    f"Unsupported onnxruntime-genai-directml-ryzenai version: {og.__version__}\n"
                    "Only RyzenAI 1.6.0 is currently supported. Please upgrade:\n"
                    "pip install --upgrade lemonade-sdk[oga-ryzenai] --extra-index-url https://pypi.amd.com/simple"
                )
        else:
            # Legacy lemonade-install approach is no longer supported
            raise ValueError(
                "Legacy RyzenAI installation detected (version < 0.7.0).\n"
                "RyzenAI 1.4.0 and 1.5.0 are no longer supported. Please upgrade to 1.6.0:\n"
                "pip install lemonade-sdk[oga-ryzenai] --extra-index-url https://pypi.amd.com/simple"
            )
    except ImportError as e:
        raise ImportError(
            f"{e}\n Please install lemonade-sdk with "
            "one of the oga extras, for example:\n"
            "pip install lemonade-sdk[dev,oga-cpu]\n"
            "See https://lemonade-server.ai/install_options.html for details"
        ) from e


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
            "--llamacpp",
            help="Install llama.cpp binaries with specified backend",
            choices=["rocm", "vulkan"],
        )

        parser.add_argument(
            "--flm",
            action="store_true",
            help="Install FLM (FastFlowLM) for running local language models",
        )

        return parser

    @staticmethod
    def _install_llamacpp(backend):
        """
        Install llama.cpp binaries with the specified backend.

        Args:
            backend: The backend to use ('rocm' or 'vulkan')
        """

        from lemonade.tools.llamacpp.utils import install_llamacpp

        install_llamacpp(backend)

    @staticmethod
    def _install_flm():
        """
        Install FLM (FastFlowLM) for running local language models.
        """

        # Check if the processor is supported before proceeding
        check_ryzen_ai_processor()

        from lemonade.tools.flm.utils import install_flm

        install_flm()

    def run(
        self,
        llamacpp: Optional[str] = None,
        flm: Optional[bool] = None,
    ):
        if llamacpp is None and flm is None:
            raise ValueError(
                "You must select something to install, "
                "for example `--llamacpp` or `--flm`"
            )

        if llamacpp is not None:
            self._install_llamacpp(llamacpp)

        if flm:
            self._install_flm()


def main():
    installer = Install()
    args = installer.parser().parse_args()
    installer.run(**args.__dict__)


if __name__ == "__main__":
    main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
