import os
import sys
import importlib.util
import importlib.metadata
import subprocess
from abc import ABC, abstractmethod
from typing import Dict, Optional


class InferenceEngineDetector:
    """
    Main class for detecting inference engine availability.
    """

    def __init__(self):
        self.oga_detector = OGADetector()
        self.llamacpp_detector = LlamaCppDetector()
        self.transformers_detector = TransformersDetector()

    def detect_engines_for_device(
        self, device_type: str, device_name: str
    ) -> Dict[str, Dict]:
        """
        Detect all available inference engines for a specific device type.

        Args:
            device_type: "cpu", "amd_igpu", "amd_dgpu", or "npu"

        Returns:
            dict: Engine availability information
        """
        engines = {}

        # Detect OGA availability
        oga_info = self.oga_detector.detect_for_device(device_type)
        if oga_info:
            engines["oga"] = oga_info

        # Detect llama.cpp vulkan availability
        llamacpp_info = self.llamacpp_detector.detect_for_device(
            device_type, device_name, "vulkan"
        )
        if llamacpp_info:
            engines["llamacpp-vulkan"] = llamacpp_info

        # Detect llama.cpp rocm availability
        llamacpp_info = self.llamacpp_detector.detect_for_device(
            device_type, device_name, "rocm"
        )
        if llamacpp_info:
            engines["llamacpp-rocm"] = llamacpp_info

        # Detect Transformers availability
        transformers_info = self.transformers_detector.detect_for_device(device_type)
        if transformers_info:
            engines["transformers"] = transformers_info

        return engines


class BaseEngineDetector(ABC):
    """
    Base class for engine-specific detectors.
    """

    @abstractmethod
    def detect_for_device(self, device_type: str) -> Optional[Dict]:
        """
        Detect engine availability for specific device type.
        """

    @abstractmethod
    def is_installed(self) -> bool:
        """
        Check if the engine package/binary is installed.
        """


class OGADetector(BaseEngineDetector):
    """
    Detector for ONNX Runtime GenAI (OGA).
    """

    def detect_for_device(self, device_type: str) -> Optional[Dict]:
        """
        Detect OGA availability for specific device.
        """
        # Check package installation based on device type
        if device_type == "npu":
            if not self.is_npu_package_installed():
                return {
                    "available": False,
                    "error": "NPU packages not installed (need "
                    "onnxruntime-genai-directml-ryzenai or onnxruntime-vitisai)",
                }
        else:
            # For other devices, check general OGA installation
            if not self.is_installed():
                return None

        try:
            import onnxruntime as ort

            # Map device types to ORT providers
            device_provider_map = {
                "cpu": "cpu",
                "amd_igpu": "dml",
                "amd_dgpu": "dml",
                "npu": "vitisai",
            }

            if device_type not in device_provider_map:
                return None

            backend = device_provider_map[device_type]

            # Map backends to ORT provider names
            provider_map = {
                "cpu": "CPUExecutionProvider",
                "dml": "DmlExecutionProvider",
                "vitisai": "VitisAIExecutionProvider",
            }

            required_provider = provider_map[backend]
            available_providers = ort.get_available_providers()

            if required_provider in available_providers:
                result = {
                    "available": True,
                    "version": self._get_oga_version(device_type),
                    "backend": backend,
                }

                # Add dependency versions in details
                result["details"] = {
                    "dependency_versions": {"onnxruntime": ort.__version__}
                }

                return result
            else:
                if device_type == "npu":
                    error_msg = (
                        "VitisAI provider not available - "
                        "check AMD NPU driver installation"
                    )
                else:
                    error_msg = f"{backend.upper()} provider not available"

                return {
                    "available": False,
                    "error": error_msg,
                }

        except (ImportError, AttributeError) as e:
            return {"available": False, "error": f"OGA detection failed: {str(e)}"}

    def is_installed(self) -> bool:
        """
        Check if OGA is installed.
        """
        return importlib.util.find_spec("onnxruntime_genai") is not None

    def is_npu_package_installed(self) -> bool:
        """
        Check if NPU-specific OGA packages are installed.
        """
        try:

            installed_packages = [
                dist.metadata["name"].lower()
                for dist in importlib.metadata.distributions()
            ]

            # Check for NPU-specific packages
            npu_packages = ["onnxruntime-genai-directml-ryzenai", "onnxruntime-vitisai"]

            for package in npu_packages:
                if package.lower() in installed_packages:
                    return True
            return False
        except (ImportError, AttributeError):
            return False

    def _get_oga_version(self, device_type: str) -> str:
        """
        Get OGA version.
        """
        try:
            # For NPU, try NPU-specific packages first
            if device_type == "npu":
                try:
                    import onnxruntime_genai_directml_ryzenai as og

                    return og.__version__
                except ImportError:
                    pass

                try:
                    import onnxruntime_vitisai as og

                    return og.__version__
                except ImportError:
                    pass

            # Fall back to general onnxruntime_genai
            import onnxruntime_genai as og

            return og.__version__
        except (ImportError, AttributeError):
            return "unknown"


class LlamaCppDetector(BaseEngineDetector):
    """
    Detector for llama.cpp.
    """

    def detect_for_device(
        self, device_type: str, device_name: str, backend: str
    ) -> Optional[Dict]:
        """
        Detect llama.cpp availability for specific device.
        """
        try:

            if device_type not in ["cpu", "amd_igpu", "amd_dgpu"]:
                return None

            # Check if the device is supported by the backend
            if device_type == "cpu":
                device_supported = True
            elif device_type == "amd_igpu" or device_type == "amd_dgpu":
                if backend == "vulkan":
                    device_supported = self._check_vulkan_support()
                elif backend == "rocm":
                    device_supported = self._check_rocm_support(device_name.lower())
            if not device_supported:
                return {"available": False, "error": f"{backend} not available"}

            is_installed = self.is_installed(backend)
            if not is_installed:
                return {
                    "available": False,
                    "error": f"{backend} binaries not installed",
                }

            return {
                "available": True,
                "version": self._get_llamacpp_version(backend),
                "backend": backend,
            }

        except (ImportError, OSError, subprocess.SubprocessError) as e:
            return {
                "available": False,
                "error": f"llama.cpp detection failed: {str(e)}",
            }

    def is_installed(self, backend: str) -> bool:
        """
        Check if llama.cpp binaries are available for any backend.
        """
        from lemonade.tools.llamacpp.utils import get_llama_server_exe_path

        try:
            server_exe_path = get_llama_server_exe_path(backend)
            if os.path.exists(server_exe_path):
                return True
        except (ImportError, OSError, ValueError):
            pass

        return False

    def _check_vulkan_support(self) -> bool:
        """
        Check if Vulkan is available for GPU acceleration.
        """
        try:
            # Run vulkaninfo to check Vulkan availability
            result = subprocess.run(
                ["vulkaninfo", "--summary"],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            return result.returncode == 0
        except (
            subprocess.TimeoutExpired,
            FileNotFoundError,
            subprocess.SubprocessError,
        ):
            try:
                # Check for Vulkan DLL on Windows
                vulkan_dll_paths = [
                    "C:\\Windows\\System32\\vulkan-1.dll",
                    "C:\\Windows\\SysWOW64\\vulkan-1.dll",
                ]
                # Check for Vulkan libraries on Linux
                vulkan_lib_paths = [
                    "/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
                    "/usr/lib/libvulkan.so.1",
                    "/lib/x86_64-linux-gnu/libvulkan.so.1",
                ]
                return any(os.path.exists(path) for path in vulkan_dll_paths) or any(
                    os.path.exists(path) for path in vulkan_lib_paths
                )
            except OSError:
                return False

    def _check_rocm_support(self, device_name: str) -> bool:
        """
        Check if ROCM is available for GPU acceleration.
        """
        from lemonade.tools.llamacpp.utils import identify_rocm_arch_from_name

        return identify_rocm_arch_from_name(device_name) is not None

    def _get_llamacpp_version(self, backend: str) -> str:
        """
        Get llama.cpp version from lemonade's managed installation for specific backend.
        """
        try:
            # Use backend-specific path - same logic as get_llama_folder_path in utils.py
            server_base_dir = os.path.join(
                os.path.dirname(sys.executable), backend, "llama_server"
            )
            version_file = os.path.join(server_base_dir, "version.txt")

            if os.path.exists(version_file):
                with open(version_file, "r", encoding="utf-8") as f:
                    version = f.read().strip()
                    return version
        except (ImportError, OSError):
            pass

        return "unknown"


class TransformersDetector(BaseEngineDetector):
    """
    Detector for Transformers/PyTorch.
    """

    def detect_for_device(self, device_type: str) -> Optional[Dict]:
        """
        Detect Transformers availability for specific device.
        """
        if not self.is_installed():
            return None

        try:
            import torch
            import transformers

            if device_type == "cpu":
                result = {
                    "available": True,
                    "version": transformers.__version__,
                    "backend": "cpu",
                }

                # Add dependency versions in details
                result["details"] = {
                    "dependency_versions": {"torch": torch.__version__}
                }

                return result
            else:
                return None

        except (ImportError, AttributeError) as e:
            return {
                "available": False,
                "error": f"Transformers detection failed: {str(e)}",
            }

    def is_installed(self) -> bool:
        """
        Check if Transformers and PyTorch are installed.
        """
        return (
            importlib.util.find_spec("transformers") is not None
            and importlib.util.find_spec("torch") is not None
        )


def detect_inference_engines(device_type: str, device_name: str) -> Dict[str, Dict]:
    """
    Helper function to detect inference engines for a device type.

    Args:
        device_type: "cpu", "amd_igpu", "amd_dgpu", or "npu"
        device_name: device name

    Returns:
        dict: Engine availability information.
    """
    detector = InferenceEngineDetector()
    return detector.detect_engines_for_device(device_type, device_name)
