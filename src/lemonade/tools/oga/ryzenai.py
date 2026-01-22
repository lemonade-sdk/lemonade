"""
RyzenAI utilities for OGA model loading.

This module provides version detection and configuration for RyzenAI installations.
"""

import os
import site
import sys

from packaging.version import Version

# NPU Driver configuration
NPU_DRIVER_DOWNLOAD_URL = (
    "https://account.amd.com/en/forms/downloads/"
    "ryzenai-eula-public-xef.html?filename=NPU_RAI1.5_280_WHQL.zip"
)
REQUIRED_NPU_DRIVER_VERSION = "32.0.203.280"

# List of supported Ryzen AI processor series (can be extended in the future)
SUPPORTED_RYZEN_AI_SERIES = ["300"]


def get_ryzenai_version_info():
    """
    Detect RyzenAI version from installed onnxruntime_genai package.

    Returns:
        Tuple of (version_string, oga_path)

    Raises:
        ValueError: If unsupported OGA version is detected
    """
    # For embedded Python on Windows, add DLL directory before using onnxruntime_genai
    # This is required to find DirectML.dll and other dependencies
    if sys.platform.startswith("win"):
        site_packages = site.getsitepackages()
        for sp in site_packages:
            oga_dir = os.path.join(sp, "onnxruntime_genai")
            if os.path.exists(oga_dir):
                os.add_dll_directory(oga_dir)
                break

    import onnxruntime_genai as og  # pylint: disable=import-outside-toplevel

    if Version(og.__version__) >= Version("0.7.0"):
        oga_path = os.path.dirname(og.__file__)
        if og.__version__ in ("0.9.2", "0.9.2.1"):
            return "1.6.0", oga_path
        else:
            raise ValueError(
                f"Unsupported onnxruntime-genai-directml-ryzenai version: {og.__version__}\n"
                "Only RyzenAI 1.6.0 is currently supported. Please upgrade:\n"
                "pip install --upgrade lemonade-sdk[oga-ryzenai] --extra-index-url https://pypi.amd.com/simple"  # pylint: disable=line-too-long
            )
    else:
        raise ValueError(
            "Legacy RyzenAI installation detected (version < 0.7.0).\n"
            "RyzenAI 1.4.0 and 1.5.0 are no longer supported. Please upgrade to 1.6.0:\n"
            "pip install lemonade-sdk[oga-ryzenai] --extra-index-url https://pypi.amd.com/simple"
        )
