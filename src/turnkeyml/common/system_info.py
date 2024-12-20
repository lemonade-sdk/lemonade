from abc import ABC
import importlib.metadata
import platform
import re
import subprocess


class SystemInfo(ABC):
    """Abstract base class for OS-dependent system information classes"""

    def __init__(self):
        pass

    def get_dict(self):
        """
        Retrieves all the system information into a dictionary

        Returns:
            dict: System information
        """
        info_dict = {
            "OS Version": self.get_os_version(),
            "Python Packages": self.get_python_packages(),
        }
        return info_dict

    @staticmethod
    def get_os_version() -> str:
        """
        Retrieves the OS version.

        Returns:
            str: OS Version
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
    """Class used to access system information in Windows"""

    def __init__(self):
        super().__init__()
        import wmi

        self.connection = wmi.WMI()

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
            str: Physical memory
        """
        memory = self.connection.Win32_PhysicalMemory()
        if memory:
            total_capacity = sum([int(m.Capacity) for m in memory])
            total_capacity_str = f"{total_capacity/(1024**3)} GB"
            details_str = " + ".join(
                [
                    f"{m.Manufacturer} {int(m.Capacity)/(1024**3)} GB {m.Speed} ns"
                    for m in memory
                ]
            )
            return total_capacity_str + " (" + details_str + ")"
        return "Physical memory information not found."

    def get_bios_version(self) -> str:
        """
        Retrieves the BIOS Version of the computer system.

        Returns:
            str: BIOS Version
        """
        bios = self.connection.Win32_BIOS()
        if bios:
            return bios[0].Name
        return "BIOS Version not found."

    def get_max_clock_speed(self) -> str:
        """
        Retrieves the max clock speed of the CPU of the system.

        Returns:
            str: Max CPU clock speed
        """
        processor = self.connection.Win32_Processor()
        if processor:
            return f"{processor[0].MaxClockSpeed} MHz"
        return "Max CPU clock speed not found."

    def get_driver_version(self, device_name) -> str:
        """
        Retrieves the driver version for the specified device name.

        Returns:
            str: Driver version, or None if device driver not found
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
            str: NPU power mode
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
            out = subprocess.check_output(["powercfg", "/getactivescheme"]).decode()
            return re.search(r"\((.*?)\)", out).group(1)
        except subprocess.CalledProcessError:
            pass
        return "Windows power setting not found"

    def get_dict(self) -> dict:
        """
        Retrieves all the system information into a dictionary

        Returns:
            dict: System information
        """
        info_dict = super().get_dict()
        info_dict["Processor"] = self.get_processor_name()
        info_dict["OEM System"] = self.get_system_model()
        info_dict["Physical Memory"] = self.get_physical_memory()
        info_dict["BIOS Version"] = self.get_bios_version()
        info_dict["CPU Max Clock"] = self.get_max_clock_speed()
        info_dict["Windows Power Setting"] = self.get_windows_power_setting()
        if "AMD" in info_dict["Processor"]:
            device_names = [
                "NPU Compute Accelerator Device",
                "AMD-OpenCL User Mode Driver",
            ]
            driver_versions = {
                device_name: self.get_driver_version(device_name)
                for device_name in device_names
            }
            info_dict["Driver Versions"] = {
                k: (v if len(v) else "DEVICE NOT FOUND")
                for k, v in driver_versions.items()
            }
            info_dict["NPU Power Mode"] = self.get_npu_power_mode()
        return info_dict


class WSLSystemInfo(SystemInfo):
    """Class used to access system information in WSL"""

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
        Retrieves all the system information into a dictionary

        Returns:
            dict: System information
        """
        info_dict = super().get_dict()
        info_dict["OEM System"] = self.get_system_model()
        return info_dict


class LinuxSystemInfo(SystemInfo):
    """Class used to access system information in Linux"""

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
            str: Physical memory
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
        Retrieves all the system information into a dictionary

        Returns:
            dict: System information
        """
        info_dict = super().get_dict()
        info_dict["Processor"] = self.get_processor_name()
        info_dict["OEM System"] = self.get_system_model()
        info_dict["Physical Memory"] = self.get_physical_memory()
        return info_dict


class UnsupportedOSSystemInfo(SystemInfo):
    """Class used to access system information in unsupported operating systems"""

    def get_dict(self):
        """
        Retrieves all the system information into a dictionary

        Returns:
            dict: System information
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
        # WSL has to be handled differently compared to native Linux
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
