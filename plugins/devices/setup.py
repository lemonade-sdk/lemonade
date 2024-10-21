import os
import json
import pathlib
from setuptools import setup


def get_specific_version(plugin_name: str, version_key: str) -> str:
    with open(
        os.path.join(
            pathlib.Path(__file__).parent,
            "src",
            "turnkeyml_plugin_devices",
            plugin_name,
            "version.json",
        ),
        "r",
        encoding="utf-8",
    ) as file:
        return json.load(file)[version_key]


setup(
    name="turnkeyml_plugin_devices",
    version="1.0.0",
    package_dir={"": "src"},
    packages=[
        "turnkeyml_plugin_devices",
        "turnkeyml_plugin_devices.common",
        "turnkeyml_plugin_devices.common.run",
        "turnkeyml_plugin_devices.onnxrt",
        "turnkeyml_plugin_devices.tensorrt",
        "turnkeyml_plugin_devices.torchrt",
    ],
    python_requires=">=3.8, <3.12",
    install_requires=[
        "turnkeyml>=4.0.0",
        "importlib_metadata",
        "onnx_tool",
        "numpy<2",
        "gitpython",
        "timm==0.9.10",
    ],
    include_package_data=True,
    package_data={"turnkeyml_plugin_devices": []},
    extras_require={
        "onnxrt": [],
        "torchrt": [],
        "tensorrt": [],
    },
)
