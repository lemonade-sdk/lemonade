import importlib.metadata
import os
from packaging.version import Version
from packaging.requirements import Requirement

from .common.run.benchmark_model import Benchmark


# Determine which plugins are available to the user, based on whether
# that plugin's requirements are satisfied in the current environment
#
# We do this by the following method:
# 1. Get the list of extras (plugins) supported by this package
# 2. Get the specific requirements for each plugin
# 3. Check whether all specific requirements for a given plugin are installed
#       in the current environment
#
# NOTE: users can set environment variable TURNKEY_PLUGIN_HELP to get
#       information about which plugins are properly installed, and the
#       reason why the other plugins are not considered to be properly
#       installed.

dist = importlib.metadata.distribution("turnkeyml_plugin_devices")
plugins_supported = dist.metadata.get_all("Provides-Extra")
dist_requirements = dist.metadata.get_all("Requires-Dist")
plugin_requirements = {plugin_name: [] for plugin_name in plugins_supported}

if os.getenv("TURNKEY_PLUGIN_HELP") == "True":
    print(
        "Plugins supported by this package, when their requirements are installed:",
        plugins_supported,
    )
    print(
        "Package requirements, including a mapping of requirements to plugins (extras):",
        dist_requirements,
    )


for req in dist_requirements:
    req_split = req.split("; extra == ")
    if len(req_split) > 1:
        plugin_name = req_split[1].replace('"', "")
        plugin_name = plugin_name.replace("'", "")
        plugin_requirements[plugin_name].append(req_split[0])

if os.getenv("TURNKEY_PLUGIN_HELP") == "True":
    print("Requirements for each plugin:", plugin_requirements)


def plugin_installed(plugin_name, plugin_reqs) -> bool:
    install_help = f"{plugin_name} is installed"
    installed = True
    for req in plugin_reqs:
        try:
            req = Requirement(req)
            pkg, ver = req.name, req.specifier
            installed_ver = Version(importlib.metadata.version(pkg))

            if installed_ver not in ver:
                installed = False
                install_help = (
                    f"{plugin_name} is not installed because, while {pkg} is installed, "
                    f"it has version {installed_ver} which does not match required version {ver}"
                )
        except importlib.metadata.PackageNotFoundError:
            installed = False
            install_help = (
                f"{plugin_name} is not installed because "
                f"requirement {pkg}{ver} is not installed."
            )
            break

    if os.getenv("TURNKEY_PLUGIN_HELP") == "True":
        print(install_help)

    return installed


installed_plugins = [
    name
    for name in plugins_supported
    if plugin_installed(name, plugin_requirements[name])
]

# Collect the total set of runtimes and tools installed by this plugin,
# given the available requirements in the environment, starting with
# those that are always available (e.g., Benchmark)
# Then, get the specific runtimes and tools made available by each of
# the installed plugins detected in the code block above
runtimes = {}
tools = [Benchmark]

if "onnxrt" in installed_plugins:
    from .onnxrt.runtime import OnnxRT

    runtimes["ort"] = {
        "RuntimeClass": OnnxRT,
        "supported_devices": {"x86"},
    }

if "torchrt" in installed_plugins:
    from .torchrt.runtime import TorchRT

    runtimes["torch-eager"] = {
        "RuntimeClass": TorchRT,
        "supported_devices": {"x86"},
    }
    runtimes["torch-compiled"] = {
        "RuntimeClass": TorchRT,
        "supported_devices": {"x86"},
    }

if "tensorrt" in installed_plugins:
    from .tensorrt.runtime import TensorRT

    runtimes["trt"] = {
        "RuntimeClass": TensorRT,
        "supported_devices": {"nvidia"},
    }

implements = {
    "runtimes": runtimes,
    "tools": tools,
}
