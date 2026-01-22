from setuptools import setup

with open("src/lemonade/version.py", encoding="utf-8") as fp:
    version = fp.read().split('"')[1]

setup(
    name="lemonade-sdk",
    version=version,
    description="Lemonade SDK: Your LLM Aide for Validation and Deployment",
    author_email="lemonade@amd.com",
    package_dir={"": "src"},
    packages=[
        "lemonade",
        "lemonade.profilers",
        "lemonade.common",
        "lemonade.tools",
        "lemonade.tools.oga",
        "lemonade.tools.report",
    ],
    install_requires=[
        # Core dependencies
        "invoke>=2.0.0",
        "onnx==1.18.0",
        "pyyaml>=5.4",
        "typeguard>=2.3.13",
        "packaging>=20.9",
        "numpy",
        "fasteners",
        "GitPython>=3.1.40",
        "psutil>=6.1.1",
        "wmi; platform_system == 'Windows'",
        "py-cpuinfo",
        "pytz",
        "zstandard",
        "openai>=2.0.0,<3.0.0",
        "transformers<=4.53.2",
        "jinja2",
        "tabulate",
        "sentencepiece",
        "huggingface-hub[hf_xet]==0.33.0",
        "python-dotenv",
        # Dependencies for benchmarking, accuracy testing, and model preparation
        "torch>=2.6.0",
        "datasets",
        "pandas>=1.5.3",
        "matplotlib",
        # Install human-eval from a forked repo with Windows support until the
        # PR (https://github.com/openai/human-eval/pull/53) is merged
        "human-eval-windows==1.0.4",
        "lm-eval[api]",
    ],
    extras_require={
        # Extras for specific backends
        "oga-ryzenai": [
            "onnxruntime-genai-directml-ryzenai==0.9.2.1",
            "protobuf>=6.30.1",
        ],
        "oga-cpu": [
            "onnxruntime-genai==0.9.2",
            "onnxruntime >=1.22.0",
        ],
        "model-generate": [
            "model-generate==1.5.0; platform_system=='Windows' and python_version=='3.10'",
        ],
    },
    classifiers=[],
    entry_points={
        "console_scripts": [
            "lemonade-eval=lemonade:lemonadecli",
        ]
    },
    python_requires=">=3.10, <3.14",
    long_description=open("README.md", "r", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    include_package_data=True,
    package_data={
        "lemonade": ["backend_versions.json"],
    },
)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
