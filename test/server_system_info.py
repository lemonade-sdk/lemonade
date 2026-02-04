"""
Tests for the /system-info endpoint with mock hardware configurations.

This test validates that recipes are correctly marked as supported/unsupported
based on the hardware configuration cached in hardware_info.json.

Each test case uses a mock cache directory with a pre-defined hardware_info.json
file representing different hardware configurations (e.g., Windows with ROCm GPU,
with NPU, without AMD GPU, etc.).

LIMITATIONS:
- OS detection is compile-time, so macOS/Linux tests will fail on Windows (and vice versa)
- CPU architecture detection is compile-time, so ARM64 tests will fail on x86_64 (and vice versa)
- Recipe/backend availability checks look at the real filesystem
- Tests are designed to run on the same platform as the server build

Usage:
    python server_system_info.py
    python server_system_info.py --server-binary /path/to/lemonade-server
"""

import json
import os
import platform
import shutil
import tempfile
import unittest
import subprocess
import time
import sys
import argparse

try:
    import requests
except ImportError as e:
    raise ImportError("You must `pip install requests` to run this test", e)

from utils.test_models import PORT, TIMEOUT_DEFAULT, get_default_server_binary
from utils.server_base import wait_for_server

# Detect current platform for skipping incompatible tests
IS_WINDOWS = sys.platform == "win32"
IS_MACOS = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")
IS_X86_64 = platform.machine().lower() in ("x86_64", "amd64")
IS_ARM64 = platform.machine().lower() in ("arm64", "aarch64")


def get_server_version(server_binary: str) -> str:
    """
    Get the lemonade server version by calling --version.

    The cache uses the version to decide if hardware needs to be re-detected.
    If the mock version doesn't match, the server will re-detect real hardware.
    """
    try:
        result = subprocess.run(
            [server_binary, "--version"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        # Output format: "lemonade-server version X.Y.Z"
        output = result.stdout.strip()
        if "version" in output.lower():
            # Extract version number
            parts = output.split()
            for part in parts:
                if part[0].isdigit():
                    return part
        return output
    except Exception as e:
        print(f"Warning: Could not get server version: {e}")
        return "9.0.0"  # Fallback


# ============================================================================
# Mock Hardware Configurations
# ============================================================================
# Each configuration represents a different system that the server might run on.
# The test creates temporary cache directories with these configurations and
# validates that the server returns the expected recipe support status.

# ============================================================================
# Mock Hardware Configurations
# ============================================================================
# Each configuration represents a different system that the server might run on.
# The test creates temporary cache directories with these configurations and
# validates that the server returns the expected recipe support status.
#
# IMPORTANT LIMITATIONS:
# - OS detection uses compile-time #ifdef, so only tests matching the build OS will work
# - CPU architecture detection uses compile-time #ifdef, so only tests matching build arch work
# - Recipe "available" status depends on what's actually installed on the test machine
#
# For each config, we specify:
# - requires_os: Which OS this test requires (test is skipped on other OSes)
# - requires_arch: Which CPU arch this test requires (test is skipped on other arches)
# - expected_supported: Backends that MUST be supported (always checked)
# - expected_unsupported: Backends that MUST be unsupported (always checked)
# - skip_available_check: Skip checking "available" since it depends on what's installed

MOCK_HARDWARE_CONFIGS = {
    # Windows x86_64 with AMD RDNA3 dGPU (ROCm-capable)
    # Tests: ROCm backend shows as supported when matching AMD GPU is present
    "windows_x86_with_rocm_dgpu": {
        "requires_os": "windows",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Windows 11 Pro 10.0.22631",
            "Processor": "AMD Ryzen 9 7950X 16-Core Processor",
            "Physical Memory": "64 GB",
            "OEM System": "Test Machine",
            "BIOS Version": "1.0.0",
            "CPU Max Clock": "5.7 GHz",
            "Windows Power Setting": "High performance",
            "devices": {
                "cpu": {
                    "name": "AMD Ryzen 9 7950X 16-Core Processor",
                    "cores": 16,
                    "threads": 32,
                    "available": True,
                },
                "amd_igpu": {"name": "None", "available": False},
                "amd_dgpu": [
                    {
                        "name": "AMD Radeon RX 7900 XTX",  # gfx1100 - RDNA3
                        "vram_gb": 24.0,
                        "driver_version": "31.0.24033.1003",
                        "available": True,
                    }
                ],
                "nvidia_dgpu": [],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "rocm", "cpu"],
            "whispercpp": ["cpu"],  # cpu backend available on x86_64
            "sd-cpp": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal"],
            "whispercpp": ["npu"],  # npu backend requires XDNA2 NPU
            # NPU recipes unsupported: CPU is "Ryzen 9 7950X" (no "Ryzen AI" -> no XDNA2)
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
    # Windows x86_64 with AMD iGPU (Strix Point - ROCm-capable) and NPU
    # Tests: ROCm iGPU support, NPU recipes (flm, oga-*)
    "windows_x86_with_rocm_igpu": {
        "requires_os": "windows",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Windows 11 Pro 10.0.22631",
            "Processor": "AMD Ryzen AI 9 HX 370",
            "Physical Memory": "32 GB",
            "OEM System": "ASUS ROG Flow Z13",
            "BIOS Version": "1.0.0",
            "CPU Max Clock": "5.1 GHz",
            "Windows Power Setting": "High performance",
            "devices": {
                "cpu": {
                    "name": "AMD Ryzen AI 9 HX 370",
                    "cores": 12,
                    "threads": 24,
                    "available": True,
                },
                "amd_igpu": {
                    "name": "AMD Radeon 890M",  # gfx1150 - Strix Point
                    "vram_gb": 8.0,
                    "available": True,
                },
                "amd_dgpu": [],
                "nvidia_dgpu": [],
                "npu": {
                    "name": "AMD NPU (Ryzen AI 300 series)",
                    "available": True,
                    "power_mode": "default",
                },
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "rocm", "cpu"],
            "whispercpp": ["npu", "cpu"],  # npu supported on XDNA2, cpu on x86_64
            "sd-cpp": ["default"],
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal"],
        },
    },
    # Windows x86_64 with no AMD GPU (Intel/NVIDIA only)
    # Tests: ROCm backend is unsupported without AMD GPU
    "windows_x86_no_amd_gpu": {
        "requires_os": "windows",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Windows 11 Pro 10.0.22631",
            "Processor": "Intel Core i9-13900K",
            "Physical Memory": "32 GB",
            "OEM System": "Dell XPS 8960",
            "BIOS Version": "1.0.0",
            "CPU Max Clock": "5.8 GHz",
            "Windows Power Setting": "Balanced",
            "devices": {
                "cpu": {
                    "name": "Intel Core i9-13900K",
                    "cores": 24,
                    "threads": 32,
                    "available": True,
                },
                "amd_igpu": {"name": "None", "available": False},
                "amd_dgpu": [],
                "nvidia_dgpu": [
                    {
                        "name": "NVIDIA GeForce RTX 4090",
                        "vram_gb": 24.0,
                        "available": True,
                    }
                ],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "cpu"],
            "whispercpp": ["cpu"],  # cpu backend available on x86_64
            "sd-cpp": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal", "rocm"],
            "whispercpp": ["npu"],  # npu backend requires XDNA2 NPU
            # NPU recipes unsupported: CPU is "Intel Core i9-13900K" (no Ryzen AI)
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
    # Windows x86_64 with AMD iGPU but NOT ROCm-capable (older GPU)
    # Tests: ROCm backend is unsupported for non-ROCm AMD GPUs
    "windows_x86_with_old_amd_igpu": {
        "requires_os": "windows",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Windows 11 Pro 10.0.22631",
            "Processor": "AMD Ryzen 7 6800U",
            "Physical Memory": "16 GB",
            "OEM System": "ASUS Zenbook",
            "BIOS Version": "1.0.0",
            "CPU Max Clock": "4.7 GHz",
            "Windows Power Setting": "Balanced",
            "devices": {
                "cpu": {
                    "name": "AMD Ryzen 7 6800U",
                    "cores": 8,
                    "threads": 16,
                    "available": True,
                },
                "amd_igpu": {
                    "name": "AMD Radeon 680M",  # gfx1030 - RDNA2, not ROCm-supported
                    "vram_gb": 2.0,
                    "available": True,
                },
                "amd_dgpu": [],
                "nvidia_dgpu": [],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "cpu"],
            "whispercpp": ["cpu"],  # cpu backend available on x86_64
            "sd-cpp": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal", "rocm"],  # rocm not supported on RDNA2
            "whispercpp": ["npu"],  # npu backend requires XDNA2 NPU
            # NPU recipes unsupported: CPU is "Ryzen 7 6800U" (no Ryzen AI)
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
    # macOS ARM64 (Apple Silicon) - ONLY RUN ON MACOS
    "macos_arm64": {
        "requires_os": "macos",
        "requires_arch": "arm64",
        "hardware_info": {
            "OS Version": "macOS 14.3.1 Darwin Kernel Version 23.3.0",
            "Processor": "Apple M3 Max",
            "Physical Memory": "64 GB",
            "devices": {
                "cpu": {
                    "name": "Apple M3 Max",
                    "cores": 14,
                    "threads": 14,
                    "available": True,
                },
                "amd_igpu": {"name": "None", "available": False},
                "amd_dgpu": [],
                "nvidia_dgpu": [],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["metal"],
        },
        "expected_unsupported": {
            "llamacpp": ["vulkan", "rocm", "cpu"],
            "whispercpp": ["npu", "cpu"],  # whispercpp is Windows-only
            "sd-cpp": ["default"],
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
    # Linux x86_64 with no AMD GPU - ONLY RUN ON LINUX
    "linux_x86_no_gpu": {
        "requires_os": "linux",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Linux-6.5.0-generic Ubuntu 22.04.3 LTS",
            "Processor": "AMD EPYC 7763 64-Core Processor",
            "Physical Memory": "256 GB",
            "devices": {
                "cpu": {
                    "name": "AMD EPYC 7763 64-Core Processor",
                    "cores": 64,
                    "threads": 128,
                    "available": True,
                },
                "amd_igpu": {"name": "None", "available": False},
                "amd_dgpu": [],
                "nvidia_dgpu": [],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "cpu"],
            "sd-cpp": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal", "rocm"],
            "whispercpp": ["npu", "cpu"],  # whispercpp is Windows-only
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
    # Linux x86_64 with AMD RDNA3 dGPU (ROCm-capable) - ONLY RUN ON LINUX
    "linux_x86_with_rocm_dgpu": {
        "requires_os": "linux",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Linux-6.5.0-generic Ubuntu 22.04.3 LTS",
            "Processor": "AMD Ryzen 9 7950X 16-Core Processor",
            "Physical Memory": "64 GB",
            "devices": {
                "cpu": {
                    "name": "AMD Ryzen 9 7950X 16-Core Processor",
                    "cores": 16,
                    "threads": 32,
                    "available": True,
                },
                "amd_igpu": {"name": "None", "available": False},
                "amd_dgpu": [
                    {
                        "name": "AMD Radeon RX 7900 XTX",  # gfx1100 - RDNA3
                        "vram_gb": 24.0,
                        "driver_version": "6.5.0",
                        "available": True,
                    }
                ],
                "nvidia_dgpu": [],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "rocm", "cpu"],
            "sd-cpp": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal"],
            "whispercpp": ["npu", "cpu"],  # whispercpp is Windows-only
            "flm": ["default"],  # Windows NPU only
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
    # Linux x86_64 with AMD GPU that doesn't support ROCm (RDNA2) - ONLY RUN ON LINUX
    "linux_x86_with_old_amd_dgpu": {
        "requires_os": "linux",
        "requires_arch": "x86_64",
        "hardware_info": {
            "OS Version": "Linux-6.5.0-generic Ubuntu 22.04.3 LTS",
            "Processor": "AMD Ryzen 9 5950X 16-Core Processor",
            "Physical Memory": "32 GB",
            "devices": {
                "cpu": {
                    "name": "AMD Ryzen 9 5950X 16-Core Processor",
                    "cores": 16,
                    "threads": 32,
                    "available": True,
                },
                "amd_igpu": {"name": "None", "available": False},
                "amd_dgpu": [
                    {
                        "name": "AMD Radeon RX 6900 XT",  # gfx1030 - RDNA2, not ROCm-supported
                        "vram_gb": 16.0,
                        "driver_version": "6.5.0",
                        "available": True,
                    }
                ],
                "nvidia_dgpu": [],
                "npu": {"name": "None", "available": False},
            },
        },
        "expected_supported": {
            "llamacpp": ["vulkan", "cpu"],
            "sd-cpp": ["default"],
        },
        "expected_unsupported": {
            "llamacpp": ["metal", "rocm"],  # rocm not supported on RDNA2
            "whispercpp": ["npu", "cpu"],  # whispercpp is Windows-only
            "flm": ["default"],
            "oga-npu": ["default"],
            "oga-hybrid": ["default"],
            "oga-cpu": ["default"],
        },
    },
}


def stop_server(server_binary):
    """Stop the lemonade server."""
    try:
        result = subprocess.run(
            [server_binary, "stop"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        # Give server time to fully stop
        time.sleep(2)
    except Exception as e:
        print(f"Warning: failed to stop server: {e}")


class SystemInfoMockTests(unittest.TestCase):
    """Tests for /system-info with mock hardware configurations."""

    @classmethod
    def setUpClass(cls):
        """Parse command line arguments and set up test configuration."""
        parser = argparse.ArgumentParser(
            description="Test system-info with mock hardware"
        )
        parser.add_argument(
            "--server-binary",
            type=str,
            default=get_default_server_binary(),
            help="Path to server binary",
        )
        args, _ = parser.parse_known_args()
        cls.server_binary = args.server_binary

        # Get the server version dynamically for cache compatibility
        cls.server_version = get_server_version(cls.server_binary)
        print(f"[SETUP] Using server version: {cls.server_version}")

        # Ensure any existing server is stopped
        stop_server(cls.server_binary)

    def check_platform_requirements(self, config: dict, config_name: str) -> bool:
        """
        Check if the current platform meets the test requirements.
        Returns True if the test can run, False if it should be skipped.

        NOTE: OS and CPU architecture detection in the C++ server uses compile-time
        #ifdef checks, NOT the cached OS Version string. This means:
        - A Windows-built server always reports "windows" as current_os
        - An x86_64-built server always reports "x86_64" as cpu family
        - Mock hardware configs can only test different GPU/NPU configurations,
          not different operating systems or CPU architectures.
        """
        requires_os = config.get("requires_os", None)
        requires_arch = config.get("requires_arch", None)

        # Check OS requirement
        if requires_os:
            current_os = "windows" if IS_WINDOWS else ("macos" if IS_MACOS else "linux")
            if requires_os != current_os:
                self.skipTest(
                    f"Server binary uses compile-time OS detection ({current_os}), "
                    f"cannot mock {requires_os} OS"
                )
                return False

        # Check architecture requirement
        if requires_arch:
            current_arch = (
                "x86_64" if IS_X86_64 else ("arm64" if IS_ARM64 else "unknown")
            )
            if requires_arch != current_arch:
                self.skipTest(
                    f"Server binary uses compile-time arch detection ({current_arch}), "
                    f"cannot mock {requires_arch} architecture"
                )
                return False

        return True

    def run_test_with_mock_hardware(self, config_name: str):
        """
        Run a single test with a mock hardware configuration.

        Creates a temporary cache directory with the mock hardware_info.json,
        starts the server with LEMONADE_CACHE_DIR pointing to it, and validates
        the /system-info response matches expected recipe support.
        """
        config = MOCK_HARDWARE_CONFIGS[config_name]

        # Check platform requirements
        self.check_platform_requirements(config, config_name)

        print(f"\n=== Testing: {config_name} ===")

        # Create temporary cache directory
        temp_cache_dir = tempfile.mkdtemp(prefix=f"lemonade_test_{config_name}_")
        print(f"Using temp cache: {temp_cache_dir}")

        try:
            # Write mock hardware_info.json with the server's version
            cache_file = os.path.join(temp_cache_dir, "hardware_info.json")
            cache_data = {
                "version": self.server_version,
                "hardware": config["hardware_info"],
            }
            with open(cache_file, "w") as f:
                json.dump(cache_data, f, indent=2)
            print(f"Created mock hardware_info.json (version={self.server_version})")

            # Prepare environment with mock cache directory
            env = os.environ.copy()
            env["LEMONADE_CACHE_DIR"] = temp_cache_dir
            # Note: Do NOT set LEMONADE_CI_MODE=1 here as it invalidates the cache!

            # Start server with mock cache
            cmd = [self.server_binary, "serve", "--no-tray", "--log-level", "debug"]
            print(f"Starting server: {' '.join(cmd)}")

            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
            )

            try:
                # Wait for server to start
                wait_for_server()
                print("Server started")

                # Call /system-info endpoint
                response = requests.get(
                    f"http://localhost:{PORT}/api/v1/system-info",
                    timeout=TIMEOUT_DEFAULT,
                )
                self.assertEqual(response.status_code, 200)

                data = response.json()
                self.assertIn(
                    "recipes", data, "Response should contain 'recipes' section"
                )

                recipes = data["recipes"]
                print(f"Recipes returned: {list(recipes.keys())}")

                # Validate expected supported backends
                for recipe, expected_backends in config.get(
                    "expected_supported", {}
                ).items():
                    self.assertIn(
                        recipe, recipes, f"Expected recipe '{recipe}' in response"
                    )

                    for backend in expected_backends:
                        backends = recipes[recipe].get("backends", {})
                        self.assertIn(
                            backend,
                            backends,
                            f"Expected backend '{backend}' for recipe '{recipe}'",
                        )
                        backend_info = backends[backend]
                        self.assertTrue(
                            backend_info.get("supported", False),
                            f"Expected {recipe}/{backend} to be supported, but got: {backend_info}",
                        )
                        print(f"  [OK] {recipe}/{backend}: supported=True")

                # Validate expected unsupported backends
                for recipe, expected_backends in config.get(
                    "expected_unsupported", {}
                ).items():
                    self.assertIn(
                        recipe,
                        recipes,
                        f"Expected recipe '{recipe}' to be in response (marked unsupported)",
                    )

                    for backend in expected_backends:
                        backends = recipes[recipe].get("backends", {})
                        self.assertIn(
                            backend,
                            backends,
                            f"Expected backend '{backend}' for recipe '{recipe}' to be in response",
                        )

                        backend_info = backends[backend]
                        self.assertFalse(
                            backend_info.get("supported", False),
                            f"Expected {recipe}/{backend} to be UNsupported, but got: {backend_info}",
                        )
                        print(f"  [OK] {recipe}/{backend}: supported=False")

            finally:
                # Stop the server
                stop_server(self.server_binary)
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()

        finally:
            # Cleanup temp directory
            try:
                shutil.rmtree(temp_cache_dir)
            except Exception as e:
                print(f"Warning: failed to cleanup temp dir: {e}")

    def test_windows_x86_with_rocm_dgpu(self):
        """Test Windows x86_64 with AMD RDNA3 dGPU (ROCm-capable)."""
        self.run_test_with_mock_hardware("windows_x86_with_rocm_dgpu")

    def test_windows_x86_with_rocm_igpu(self):
        """Test Windows x86_64 with AMD Strix Point iGPU and NPU."""
        self.run_test_with_mock_hardware("windows_x86_with_rocm_igpu")

    def test_windows_x86_no_amd_gpu(self):
        """Test Windows x86_64 with no AMD GPU (Intel/NVIDIA only)."""
        self.run_test_with_mock_hardware("windows_x86_no_amd_gpu")

    def test_windows_x86_with_old_amd_igpu(self):
        """Test Windows x86_64 with AMD iGPU that doesn't support ROCm."""
        self.run_test_with_mock_hardware("windows_x86_with_old_amd_igpu")

    def test_macos_arm64(self):
        """Test macOS ARM64 (Apple Silicon) - requires macOS-built server."""
        self.run_test_with_mock_hardware("macos_arm64")

    def test_linux_x86_no_gpu(self):
        """Test Linux x86_64 with no AMD GPU - requires Linux-built server."""
        self.run_test_with_mock_hardware("linux_x86_no_gpu")

    def test_linux_x86_with_rocm_dgpu(self):
        """Test Linux x86_64 with AMD RDNA3 dGPU (ROCm-capable) - requires Linux-built server."""
        self.run_test_with_mock_hardware("linux_x86_with_rocm_dgpu")

    def test_linux_x86_with_old_amd_dgpu(self):
        """Test Linux x86_64 with AMD RDNA2 dGPU (no ROCm) - requires Linux-built server."""
        self.run_test_with_mock_hardware("linux_x86_with_old_amd_dgpu")


if __name__ == "__main__":
    # Run tests
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(SystemInfoMockTests)

    runner = unittest.TextTestRunner(verbosity=2, buffer=False)
    result = runner.run(suite)

    sys.exit(0 if result.wasSuccessful() else 1)
