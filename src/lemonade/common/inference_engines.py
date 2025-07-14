import os
import sys
import importlib.util
import importlib.metadata
import platform
import subprocess
from abc import ABC, abstractmethod
from typing import Dict, Optional
import transformers


class InferenceEngineDetector:
    """
    Main class for detecting inference engine availability.
    """

    def __init__(self):
        self.oga_detector = OGADetector()
        self.llamacpp_detector = LlamaCppDetector()
        self.transformers_detector = TransformersDetector()

    def detect_engines_for_device(self, device_type: str) -> Dict[str, Dict]:
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

        # Detect llama.cpp availability
        llamacpp_info = self.llamacpp_detector.detect_for_device(device_type)
        if llamacpp_info:
            engines["llamacpp"] = llamacpp_info

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

    def detect_for_device(self, device_type: str) -> Optional[Dict]:
        """
        Detect llama.cpp availability for specific device.
        """
        try:
            # Map device types to llama.cpp backends
            device_backend_map = {
                "cpu": "cpu",
                "amd_igpu": "vulkan",
                "amd_dgpu": "vulkan",
            }

            if device_type not in device_backend_map:
                return None

            backend = device_backend_map[device_type]
            is_installed = self.is_installed()

            # Check requirements based on backend
            if backend == "vulkan":
                vulkan_available = self._check_vulkan_support()
                if not vulkan_available:
                    return {"available": False, "error": "Vulkan not available"}

                # Vulkan is available
                if is_installed:
                    result = {
                        "available": True,
                        "version": self._get_llamacpp_version(),
                        "backend": backend,
                    }
                    return result
                else:
                    return {
                        "available": False,
                        "error": "llama.cpp binaries not installed",
                    }
            else:
                # CPU backend
                if is_installed:
                    result = {
                        "available": True,
                        "version": self._get_llamacpp_version(),
                        "backend": backend,
                    }
                    return result
                else:
                    return {
                        "available": False,
                        "error": "llama.cpp binaries not installed",
                    }

        except (ImportError, OSError, subprocess.SubprocessError) as e:
            return {
                "available": False,
                "error": f"llama.cpp detection failed: {str(e)}",
            }

    def is_installed(self) -> bool:
        """
        Check if llama.cpp binaries are available.
        """

        # Check lemonade-managed binary locations
        try:

            # Check lemonade server directory
            server_base_dir = os.path.join(
                os.path.dirname(sys.executable), "llama_server"
            )

            if platform.system().lower() == "windows":
                server_exe_path = os.path.join(server_base_dir, "llama-server.exe")
            else:
                # Check both build/bin and root directory locations
                build_bin_path = os.path.join(
                    server_base_dir, "build", "bin", "llama-server"
                )
                root_path = os.path.join(server_base_dir, "llama-server")
                server_exe_path = (
                    build_bin_path if os.path.exists(build_bin_path) else root_path
                )

            if os.path.exists(server_exe_path):
                return True

        except (ImportError, OSError):
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

    def _get_llamacpp_version(self) -> str:
        """
        Get llama.cpp version from lemonade's managed installation.
        """
        try:
            server_base_dir = os.path.join(
                os.path.dirname(sys.executable), "llama_server"
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


def detect_inference_engines(device_type: str) -> Dict[str, Dict]:
    """
    Helper function to detect inference engines for a device type.

    Args:
        device_type: "cpu", "amd_igpu", "amd_dgpu", or "npu"

    Returns:
        dict: Engine availability information.
    """
    detector = InferenceEngineDetector()
    return detector.detect_engines_for_device(device_type)
