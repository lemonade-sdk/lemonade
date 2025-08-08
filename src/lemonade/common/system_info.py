from abc import ABC, abstractmethod
import importlib.metadata
import platform
import re
import subprocess
import ctypes
from .inference_engines import detect_inference_engines

# AMD GPU classification keywords - shared across all OS implementations
# If a GPU name contains any of these keywords, it's considered discrete
# Everything else is assumed to be integrated
AMD_DISCRETE_GPU_KEYWORDS = [
    "rx ",
    "xt",
    "pro w",
    "pro v",
    "radeon pro",
    "firepro",
    "fury",
]


class SystemInfo(ABC):
    """
    Abstract base class for OS-dependent system information classes.
    """

    def __init__(self):
        pass

    def get_dict(self):
        """
        Retrieves all the system information into a dictionary.

        Returns:
            dict: System information.
        """
        info_dict = {
            "OS Version": self.get_os_version(),
        }
        return info_dict

    def get_device_dict(self):
        """
        Retrieves device information into a dictionary.

        Returns:
            dict: Device information.
        """
        device_dict = {
            "cpu": self.get_cpu_device(),
            "amd_igpu": self.get_amd_igpu_device(include_inference_engines=True),
            "amd_dgpu": self.get_amd_dgpu_devices(include_inference_engines=True),
            "npu": self.get_npu_device(),
        }
        return device_dict

    @abstractmethod
    def get_cpu_device(self) -> dict:
        """
        Retrieves CPU device information.

        Returns:
            dict: CPU device information.
        """

    @abstractmethod
    def get_amd_igpu_device(self, include_inference_engines: bool = False) -> dict:
        """
        Retrieves AMD integrated GPU device information.

        Returns:
            dict: AMD iGPU device information.
        """

    @abstractmethod
    def get_amd_dgpu_devices(self, include_inference_engines: bool = False) -> list:
        """
        Retrieves AMD discrete GPU device information.

        Returns:
            list: List of AMD dGPU device information.
        """

    @abstractmethod
    def get_npu_device(self) -> dict:
        """
        Retrieves NPU device information.

        Returns:
            dict: NPU device information.
        """

    @staticmethod
    def get_os_version() -> str:
        """
        Retrieves the OS version.

        Returns:
            str: OS Version.
        """
        try:
            return platform.platform()
        except Exception as e:  # pylint: disable=broad-except
            return f"ERROR - {e}"

    @staticmethod
    def get_python_packages() -> list:
        """
        Retrieves the Python package versions.

        Returns:
            list: List of Python package versions in the form ["package-name==package-version", ...]
        """
        # Get Python Packages
        distributions = importlib.metadata.distributions()
        return [
            f"{dist.metadata['name']}=={dist.metadata['version']}"
            for dist in distributions
        ]


class WindowsSystemInfo(SystemInfo):
    """
    Class used to access system information in Windows.
    """

    def __init__(self):
        super().__init__()
        import wmi

        self.connection = wmi.WMI()

    def get_cpu_device(self) -> dict:
        """
        Retrieves CPU device information using WMI.

        Returns:
            dict: CPU device information.
        """
        try:
            processors = self.connection.Win32_Processor()
            if processors:
                processor = processors[0]
                cpu_name = processor.Name.strip()
                cpu_info = {
                    "name": cpu_name,
                    "cores": processor.NumberOfCores,
                    "threads": processor.NumberOfLogicalProcessors,
                    "max_clock_speed_mhz": processor.MaxClockSpeed,
                    "available": True,
                }

                # Add inference engine detection
                cpu_info["inference_engines"] = self._detect_inference_engines(
                    "cpu", cpu_name
                )
                return cpu_info

        except Exception as e:  # pylint: disable=broad-except
            return {"available": False, "error": f"CPU detection failed: {e}"}

        return {"available": False, "error": "No CPU information found"}

    def _detect_amd_gpus(self, gpu_type: str, include_inference_engines: bool = False):
        """
        Shared AMD GPU detection logic for both integrated and discrete GPUs.
        Uses keyword-based classification for simplicity and reliability.

        Args:
            gpu_type: Either "integrated" or "discrete"

        Returns:
            list: List of detected GPU info dictionaries
        """
        gpu_devices = []
        try:
            video_controllers = self.connection.Win32_VideoController()
            for controller in video_controllers:
                if (
                    controller.Name
                    and "AMD" in controller.Name
                    and "Radeon" in controller.Name
                ):

                    name_lower = controller.Name.lower()

                    # Keyword-based classification - simple and reliable
                    is_discrete_by_name = any(
                        kw in name_lower for kw in AMD_DISCRETE_GPU_KEYWORDS
                    )
                    is_integrated = not is_discrete_by_name

                    # Filter based on requested type
                    if (gpu_type == "integrated" and is_integrated) or (
                        gpu_type == "discrete" and not is_integrated
                    ):

                        device_type = "amd_igpu" if is_integrated else "amd_dgpu"
                        gpu_info = {
                            "name": controller.Name,
                            "available": True,
                        }

                        driver_version = self.get_driver_version(
                            "AMD-OpenCL User Mode Driver"
                        )
                        gpu_info["driver_version"] = (
                            driver_version if driver_version else "Unknown"
                        )

                        if include_inference_engines:
                            gpu_info["inference_engines"] = (
                                self._detect_inference_engines(
                                    device_type, controller.Name
                                )
                            )
                        gpu_devices.append(gpu_info)

        except Exception as e:  # pylint: disable=broad-except
            error_msg = f"AMD {gpu_type} GPU detection failed: {e}"
            return [{"available": False, "error": error_msg}]

        return gpu_devices

    def get_amd_igpu_device(self, include_inference_engines: bool = False) -> dict:
        """
        Retrieves AMD integrated GPU device information using keyword-based classification.

        Returns:
            dict: AMD iGPU device information.
        """
        igpu_devices = self._detect_amd_gpus(
            "integrated", include_inference_engines=include_inference_engines
        )
        return (
            igpu_devices[0]
            if igpu_devices
            else {"available": False, "error": "No AMD integrated GPU found"}
        )

    def get_amd_dgpu_devices(self, include_inference_engines: bool = False):
        """
        Retrieves AMD discrete GPU device information using keyword-based classification.

        Returns:
            list: List of AMD dGPU device information.
        """
        dgpu_devices = self._detect_amd_gpus(
            "discrete", include_inference_engines=include_inference_engines
        )
        return (
            dgpu_devices
            if dgpu_devices
            else [{"available": False, "error": "No AMD discrete GPU found"}]
        )

    def get_npu_device(self) -> dict:
        """
        Retrieves NPU device information using existing methods.

        Returns:
            dict: NPU device information.
        """
        try:
            # Check if NPU driver is present
            driver_version = self.get_driver_version("NPU Compute Accelerator Device")
            if driver_version:
                power_mode = self.get_npu_power_mode()
                npu_info = {
                    "name": "AMD NPU",
                    "driver_version": driver_version,
                    "power_mode": power_mode,
                    "available": True,
                }

                # Add inference engine detection
                npu_info["inference_engines"] = self._detect_inference_engines(
                    "npu", "AMD NPU"
                )
                return npu_info
        except Exception as e:  # pylint: disable=broad-except
            return {"available": False, "error": f"NPU detection failed: {e}"}

        return {"available": False, "error": "No NPU device found"}

    def get_processor_name(self) -> str:
        """
        Retrieves the name of the processor.

        Returns:
            str: Name of the processor.
        """
        processors = self.connection.Win32_Processor()
        if processors:
            return (
                f"{processors[0].Name.strip()} "
                f"({processors[0].NumberOfCores} cores, "
                f"{processors[0].NumberOfLogicalProcessors} logical processors)"
            )
        return "Processor information not found."

    def get_basic_processor_name(self) -> str:
        """
        Retrieves the basic name of the processor without core/thread details.

        Returns:
            str: Basic name of the processor.
        """
        processors = self.connection.Win32_Processor()
        if processors:
            return processors[0].Name.strip()
        return "Processor information not found."

    def get_system_model(self) -> str:
        """
        Retrieves the model of the computer system.

        Returns:
            str: Model of the computer system.
        """
        systems = self.connection.Win32_ComputerSystem()
        if systems:
            return systems[0].Model
        return "System model information not found."

    def get_physical_memory(self) -> str:
        """
        Retrieves the physical memory of the computer system.

        Returns:
            str: Physical memory.
        """
        memory = self.connection.Win32_PhysicalMemory()
        if memory:
            total_capacity = sum([int(m.Capacity) for m in memory])
            total_capacity_str = f"{total_capacity/(1024**3)} GB"
            return total_capacity_str
        return "Physical memory information not found."

    def get_bios_version(self) -> str:
        """
        Retrieves the BIOS Version of the computer system.

        Returns:
            str: BIOS Version.
        """
        bios = self.connection.Win32_BIOS()
        if bios:
            return bios[0].Name
        return "BIOS Version not found."

    def get_max_clock_speed(self) -> str:
        """
        Retrieves the max clock speed of the CPU of the system.

        Returns:
            str: Max CPU clock speed.
        """
        processor = self.connection.Win32_Processor()
        if processor:
            return f"{processor[0].MaxClockSpeed} MHz"
        return "Max CPU clock speed not found."

    def get_driver_version(self, device_name) -> str:
        """
        Retrieves the driver version for the specified device name.

        Returns:
            str: Driver version, or None if device driver not found.
        """
        drivers = self.connection.Win32_PnPSignedDriver(DeviceName=device_name)
        if drivers:
            return drivers[0].DriverVersion
        return ""

    @staticmethod
    def get_npu_power_mode() -> str:
        """
        Retrieves the NPU power mode.

        Returns:
            str: NPU power mode.
        """
        try:
            out = subprocess.check_output(
                [
                    r"C:\Windows\System32\AMD\xrt-smi.exe",
                    "examine",
                    "-r",
                    "platform",
                ],
                stderr=subprocess.STDOUT,
            ).decode()
            lines = out.splitlines()
            modes = [line.split()[-1] for line in lines if "Mode" in line]
            if len(modes) > 0:
                return modes[0]
        except FileNotFoundError:
            # xrt-smi not present
            pass
        except subprocess.CalledProcessError:
            pass
        return "NPU power mode not found."

    @staticmethod
    def get_windows_power_setting() -> str:
        """
        Retrieves the Windows power setting.

        Returns:
            str: Windows power setting.
        """
        try:
            # Capture output as bytes
            out_bytes = subprocess.check_output(["powercfg", "/getactivescheme"])

            # Get system's OEM code page (e.g., cp437, cp850)
            oem_cp = "cp" + str(ctypes.windll.kernel32.GetOEMCP())

            # Decode using detected OEM code page
            out = out_bytes.decode(oem_cp)

            # Extract power scheme name from parentheses
            match = re.search(r"\((.*?)\)", out)
            if match:
                return match.group(1)
            return "Power scheme name not found in output"

        except subprocess.CalledProcessError:
            return "Windows power setting not found (command failed)"
        except Exception as e:  # pylint: disable=broad-except
            return f"Error retrieving power setting: {str(e)}"

    def get_dict(self) -> dict:
        """
        Retrieves all the system information into a dictionary.

        Returns:
            dict: System information.
        """
        info_dict = super().get_dict()
        info_dict["Processor"] = self.get_basic_processor_name()
        info_dict["OEM System"] = self.get_system_model()
        info_dict["Physical Memory"] = self.get_physical_memory()
        info_dict["BIOS Version"] = self.get_bios_version()
        info_dict["CPU Max Clock"] = self.get_max_clock_speed()
        info_dict["Windows Power Setting"] = self.get_windows_power_setting()
        return info_dict

    def _detect_inference_engines(self, device_type: str, device_name: str) -> dict:
        """
        Detect available inference engines for a specific device type.

        Args:
            device_type: Device type ("cpu", "amd_igpu", "amd_dgpu", "npu")
            device_name: Device name

        Returns:
            dict: Available inference engines and their information.
        """
        try:
            from .inference_engines import detect_inference_engines

            return detect_inference_engines(device_type, device_name)
        except Exception as e:  # pylint: disable=broad-except
            return {"error": f"Inference engine detection failed: {str(e)}"}


class WSLSystemInfo(SystemInfo):
    """
    Class used to access system information in WSL.
    """

    def get_cpu_device(self) -> dict:
        """
        Retrieves CPU device information in WSL environment.
        """
        return {"available": False, "error": "Device detection not supported in WSL"}

    def get_amd_igpu_device(self, include_inference_engines: bool = False) -> dict:
        """
        Retrieves AMD integrated GPU device information in WSL environment.
        """
        return {"available": False, "error": "GPU detection not supported in WSL"}

    def get_amd_dgpu_devices(self, include_inference_engines: bool = False) -> list:
        """
        Retrieves AMD discrete GPU device information in WSL environment.
        """
        return []

    def get_npu_device(self) -> dict:
        """
        Retrieves NPU device information in WSL environment.
        """
        return {"available": False, "error": "NPU detection not supported in WSL"}

    @staticmethod
    def get_system_model() -> str:
        """
        Retrieves the model of the computer system.

        Returns:
            str: Model of the computer system.
        """
        try:
            oem_info = (
                subprocess.check_output(
                    'powershell.exe -Command "wmic computersystem get model"',
                    shell=True,
                )
                .decode()
                .strip()
            )
            oem_info = (
                oem_info.replace("\r", "").replace("\n", "").split("Model")[-1].strip()
            )
            return oem_info
        except Exception as e:  # pylint: disable=broad-except
            return f"ERROR - {e}"

    def get_dict(self) -> dict:
        """
        Retrieves all the system information into a dictionary.

        Returns:
            dict: System information.
        """
        info_dict = super().get_dict()
        info_dict["OEM System"] = self.get_system_model()
        return info_dict


class LinuxSystemInfo(SystemInfo):
    """
    Class used to access system information in Linux.
    """

    def get_cpu_device(self) -> dict:
        """
        Retrieves CPU device information using /proc/cpuinfo and lscpu.

        Returns:
            dict: CPU device information.
        """
        try:
            cpu_info = subprocess.check_output("lscpu", shell=True).decode()
            cpu_data = {}

            for line in cpu_info.split("\n"):
                if "Model name:" in line:
                    cpu_data["name"] = line.split(":")[1].strip()
                elif "CPU(s):" in line and "NUMA" not in line:
                    cpu_data["threads"] = int(line.split(":")[1].strip())
                elif "Core(s) per socket:" in line:
                    cores_per_socket = int(line.split(":")[1].strip())
                    sockets = (
                        1  # Default to 1 socket as most laptops have a single socket
                    )
                    for l in cpu_info.split("\n"):
                        if "Socket(s):" in l:
                            sockets = int(l.split(":")[1].strip())
                            break
                    cpu_data["cores"] = cores_per_socket * sockets
                elif "Architecture:" in line:
                    cpu_data["architecture"] = line.split(":")[1].strip()

            if "name" in cpu_data:
                cpu_name = cpu_data.get("name", "Unknown")
                cpu_info = {
                    "name": cpu_data.get("name", "Unknown"),
                    "cores": cpu_data.get("cores", "Unknown"),
                    "threads": cpu_data.get("threads", "Unknown"),
                    "architecture": cpu_data.get("architecture", "Unknown"),
                    "available": True,
                }

                # Add inference engine detection
                cpu_info["inference_engines"] = self._detect_inference_engines(
                    "cpu", cpu_name
                )
                return cpu_info
        except Exception as e:  # pylint: disable=broad-except
            return {"available": False, "error": f"CPU detection failed: {e}"}

        return {"available": False, "error": "No CPU information found"}

    def _detect_amd_gpus(self, gpu_type: str, include_inference_engines: bool = False):
        """
        Shared AMD GPU detection logic for both integrated and discrete GPUs.
        Uses keyword-based classification for simplicity and reliability.

        Args:
            gpu_type: Either "integrated" or "discrete".

        Returns:
            list: List of detected GPU info dictionaries.
        """
        gpu_devices = []
        try:
            lspci_output = subprocess.check_output(
                "lspci | grep -i 'vga\\|3d\\|display'", shell=True
            ).decode()

            for line in lspci_output.split("\n"):
                if line.strip() and "AMD" in line:
                    name_lower = line.lower()

                    # Keyword-based classification - simple and reliable
                    is_discrete_by_name = any(
                        kw in name_lower for kw in AMD_DISCRETE_GPU_KEYWORDS
                    )
                    is_integrated = not is_discrete_by_name

                    # Filter based on requested type
                    if (gpu_type == "integrated" and is_integrated) or (
                        gpu_type == "discrete" and not is_integrated
                    ):

                        device_type = "amd_igpu" if is_integrated else "amd_dgpu"
                        device_name = line.split(": ")[1] if ": " in line else line

                        gpu_info = {
                            "name": device_name,
                            "available": True,
                        }
                        if include_inference_engines:
                            gpu_info["inference_engines"] = (
                                self._detect_inference_engines(device_type, device_name)
                            )
                        gpu_devices.append(gpu_info)

        except Exception as e:  # pylint: disable=broad-except
            error_msg = f"AMD {gpu_type} GPU detection failed: {e}"
            return [{"available": False, "error": error_msg}]

        return gpu_devices

    def get_amd_igpu_device(self, include_inference_engines: bool = False) -> dict:
        """
        Retrieves AMD integrated GPU device information using keyword-based classification.

        Returns:
            dict: AMD iGPU device information.
        """
        igpu_devices = self._detect_amd_gpus(
            "integrated", include_inference_engines=include_inference_engines
        )
        return (
            igpu_devices[0]
            if igpu_devices
            else {"available": False, "error": "No AMD integrated GPU found"}
        )

    def get_amd_dgpu_devices(self, include_inference_engines: bool = False) -> list:
        """
        Retrieves AMD discrete GPU device information using keyword-based classification.

        Returns:
            list: List of AMD dGPU device information.
        """
        dgpu_devices = self._detect_amd_gpus(
            "discrete", include_inference_engines=include_inference_engines
        )
        return (
            dgpu_devices
            if dgpu_devices
            else [{"available": False, "error": "No AMD discrete GPU found"}]
        )

    def get_npu_device(self) -> dict:
        """
        Retrieves NPU device information (limited support on Linux).

        Returns:
            dict: NPU device information.
        """
        return {
            "available": False,
            "error": "NPU detection not yet implemented for Linux",
        }

    @staticmethod
    def get_processor_name() -> str:
        """
        Retrieves the name of the processor.

        Returns:
            str: Name of the processor.
        """
        # Get CPU Information
        try:
            cpu_info = subprocess.check_output("lscpu", shell=True).decode()
            for line in cpu_info.split("\n"):
                if "Model name:" in line:
                    return line.split(":")[1].strip()
        except Exception as e:  # pylint: disable=broad-except
            return f"ERROR - {e}"

    @staticmethod
    def get_system_model() -> str:
        """
        Retrieves the model of the computer system.

        Returns:
            str: Model of the computer system.
        """
        # Get OEM System Information
        try:
            oem_info = (
                subprocess.check_output(
                    "sudo -n dmidecode -s system-product-name",
                    shell=True,
                    stderr=subprocess.DEVNULL,
                )
                .decode()
                .strip()
                .replace("\n", " ")
            )
            return oem_info
        except subprocess.CalledProcessError:
            # This catches the case where sudo requires a password
            return "Unable to get oem info - password required"
        except Exception as e:  # pylint: disable=broad-except
            return f"ERROR - {e}"

    @staticmethod
    def get_physical_memory() -> str:
        """
        Retrieves the physical memory of the computer system.

        Returns:
            str: Physical memory.
        """
        try:
            mem_info = (
                subprocess.check_output("free -m", shell=True)
                .decode()
                .split("\n")[1]
                .split()[1]
            )
            mem_info_gb = round(int(mem_info) / 1024, 2)
            return f"{mem_info_gb} GB"
        except Exception as e:  # pylint: disable=broad-except
            return f"ERROR - {e}"

    def get_dict(self) -> dict:
        """
        Retrieves all the system information into a dictionary.

        Returns:
            dict: System information.
        """
        info_dict = super().get_dict()
        info_dict["Processor"] = self.get_processor_name()
        info_dict["OEM System"] = self.get_system_model()
        info_dict["Physical Memory"] = self.get_physical_memory()
        return info_dict

    def _detect_inference_engines(self, device_type: str, device_name: str) -> dict:
        """
        Detect available inference engines for a specific device type.

        Args:
            device_type: Device type ("cpu", "amd_igpu", "amd_dgpu", "npu")

        Returns:
            dict: Available inference engines and their information.
        """
        try:
            return detect_inference_engines(device_type, device_name)
        except Exception as e:  # pylint: disable=broad-except
            return {"error": f"Inference engine detection failed: {str(e)}"}


class UnsupportedOSSystemInfo(SystemInfo):
    """
    Class used to access system information in unsupported operating systems.
    """

    def get_cpu_device(self) -> dict:
        """
        Retrieves CPU device information for unsupported OS.
        """
        return {
            "available": False,
            "error": "Device detection not supported on this operating system",
        }

    def get_amd_igpu_device(self, include_inference_engines: bool = False) -> dict:
        """
        Retrieves AMD integrated GPU device information for unsupported OS.
        """
        return {
            "available": False,
            "error": "Device detection not supported on this operating system",
        }

    def get_amd_dgpu_devices(self, include_inference_engines: bool = False) -> list:
        """
        Retrieves AMD discrete GPU device information for unsupported OS.
        """
        return []

    def get_npu_device(self) -> dict:
        """
        Retrieves NPU device information for unsupported OS.
        """
        return {
            "available": False,
            "error": "Device detection not supported on this operating system",
        }

    def get_dict(self):
        """
        Retrieves all the system information into a dictionary.

        Returns:
            dict: System information.
        """
        info_dict = super().get_dict()
        info_dict["Error"] = "UNSUPPORTED OS"
        return info_dict


def get_system_info() -> SystemInfo:
    """
    Creates the appropriate SystemInfo object based on the operating system.

    Returns:
        A subclass of SystemInfo for the current operating system.
    """
    os_type = platform.system()
    if os_type == "Windows":
        return WindowsSystemInfo()
    elif os_type == "Linux":
        # WSL has to be handled differently compared to native Linux.
        if "microsoft" in str(platform.release()):
            return WSLSystemInfo()
        else:
            return LinuxSystemInfo()
    else:
        return UnsupportedOSSystemInfo()


def get_system_info_dict() -> dict:
    """
    Puts the system information into a dictionary.

    Returns:
        dict: Dictionary containing the system information.
    """
    return get_system_info().get_dict()


def get_device_info_dict() -> dict:
    """
    Puts the device information into a dictionary.

    Returns:
        dict: Dictionary containing the device information.
    """
    return get_system_info().get_device_dict()


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
