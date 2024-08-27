from typing import Optional
from typing import List, Dict, Tuple
import turnkeyml.common.plugins as plugins
from turnkeyml.sequence import Sequence
import turnkeyml.common.exceptions as exp


DEFAULT_RUNTIME = 0


def supported_devices_list(data: Dict, parent_key: str = "") -> List:
    """Recursive function to generate all device::part::config pairs"""
    result = []
    for key, value in data.items():
        current_key = f"{parent_key}::{key}" if parent_key else key
        result.append(current_key)
        if isinstance(value, dict):
            result.extend(supported_devices_list(value, current_key))
        elif isinstance(value, list):
            for item in value:
                result.append(f"{current_key}::{item}")
    return result


def runtime_plugins():

    plugin_modules = plugins.discover().values()

    supported_runtimes = {}

    for module in plugin_modules:
        if "runtimes" in module.implements.keys():
            for runtime_name, runtime_info in module.implements["runtimes"].items():
                if runtime_name in supported_runtimes:
                    raise ValueError(
                        f"Your turnkeyml installation has two runtimes named '{runtime_name}' "
                        "installed. You must uninstall one of your plugins that includes "
                        f"{runtime_name}. Your imported runtime plugins are: {supported_runtimes}\n"
                        f"This error was thrown while trying to import {module}"
                    )
                if isinstance(runtime_info["supported_devices"], set):
                    runtime_info["supported_devices"] = {
                        item: {} for item in runtime_info["supported_devices"]
                    }
                supported_runtimes[runtime_name] = runtime_info

    # Get the list of supported devices by checking which devices each runtime supports
    supported_devices = []
    for runtime_info in supported_runtimes.values():
        supported_devices.extend(
            supported_devices_list(runtime_info["supported_devices"])
        )
    supported_devices = list(set(supported_devices))

    # Organizing the supported devices that are in the "long form".
    # Those are not the elements used for setting the defaults.
    # This is useful for nicely showing the list of supported devices as part of the help menu.
    supported_devices.sort()

    # Create a map of devices to the runtimes that support them
    device_runtime_map = {key: [] for key in supported_devices}
    for device in supported_devices:
        for runtime_name, runtime_info in supported_runtimes.items():
            if device in supported_devices_list(runtime_info["supported_devices"]):
                device_runtime_map[device].append(runtime_name)

    return supported_devices, supported_runtimes, device_runtime_map


def apply_default_runtime(device: str, runtime: Optional[str] = None):
    _, _, device_runtime_map = runtime_plugins()

    if runtime is None:
        return device_runtime_map[str(device)][DEFAULT_RUNTIME]
    else:
        return runtime


def _check_suggestion(value: str):
    return (
        f"You may need to check the spelling of '{value}', install a "
        "plugin, or update the turnkeyml package."
    )


def select_runtime(device: str, runtime: Optional[str]) -> Tuple[str, str, Sequence]:
    supported_devices, supported_runtimes, device_runtime_map = runtime_plugins()

    # Convert to str in case its an instance of Device
    device_str = str(device)

    selected_runtime = apply_default_runtime(device_str, runtime)

    # Validate device and runtime selections
    if device_str not in supported_devices:
        raise exp.ArgError(
            f"Device argument '{device_str}' is not one of the available "
            f"supported devices {supported_devices}\n"
            f"{_check_suggestion(device_str)}"
        )
    if selected_runtime not in device_runtime_map[device_str]:
        raise exp.ArgError(
            f"Runtime argument '{selected_runtime}' is not one of the available "
            f"runtimes supported for device '{device_str}': {device_runtime_map[device_str]}\n"
            f"{_check_suggestion(selected_runtime)}"
        )

    # Get the plugin module for the selected runtime
    runtime_info = supported_runtimes[selected_runtime]

    return selected_runtime, runtime_info
