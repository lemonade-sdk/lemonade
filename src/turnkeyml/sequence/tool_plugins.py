import turnkeyml.tools.export as export
import turnkeyml.tools.onnx as onnx_tools
import turnkeyml.common.plugins as plugins
import turnkeyml.tools.management_tools as mgmt
from turnkeyml.run.benchmark_model import Benchmark
from turnkeyml.tools.discovery import Discover
import turnkeyml.tools.report as report
from turnkeyml.tools.load_build import LoadBuild

# Plugin interface for sequences
discovered_plugins = plugins.discover()

# Populated supported sequences dict with builtin sequences
SUPPORTED_TOOLS = [
    mgmt.Version,
    mgmt.Cache,
    mgmt.ModelsLocation,
    report.Report,
    Benchmark,
    Discover,
    export.ExportPytorchModel,
    onnx_tools.OptimizeOnnxModel,
    onnx_tools.LoadOnnx,
    onnx_tools.ConvertOnnxToFp16,
    export.VerifyOnnxExporter,
    LoadBuild,
]

# Add sequences from plugins to supported sequences dict
for module in discovered_plugins.values():
    if "tools" in module.implements.keys():
        for tool_class in module.implements["tools"]:
            if tool_class in SUPPORTED_TOOLS:
                name = tool_class.__class__.unique_name
                raise ValueError(
                    f"Your turnkeyml installation has two tools named '{name}' "
                    "installed. You must uninstall one of your plugins that includes "
                    f"{name}. Your imported sequence plugins are: {SUPPORTED_TOOLS}\n"
                    f"This error was thrown while trying to import {module}"
                )

            SUPPORTED_TOOLS.append(tool_class)
