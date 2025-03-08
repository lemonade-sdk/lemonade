from setuptools import setup

with open("src/turnkeyml/version.py", encoding="utf-8") as fp:
    version = fp.read().split('"')[1]


setup(
    name="turnkeyml",
    version=version,
    description="TurnkeyML Tools and Models",
    author_email="turnkeyml@amd.com",
    package_dir={"": "src", "turnkeyml_models": "models"},
    packages=[
        "turnkeyml",
        "turnkeyml.tools",
        "turnkeyml.tools.discovery",
        "turnkeyml.sequence",
        "turnkeyml.cli",
        "turnkeyml.common",
        "lemonade",
        "lemonade.tools",
        "lemonade.tools.ort_genai",
        "lemonade.tools.quark",
        "lemonade.tools.report",
        "turnkeyml_models",
        "turnkeyml_models.graph_convolutions",
        "turnkeyml_models.selftest",
        "turnkeyml_models.timm",
        "turnkeyml_models.torch_hub",
        "turnkeyml_models.torchvision",
        "turnkeyml_models.transformers",
        "lemonade_install",
    ],
    install_requires=[
        "invoke>=2.0.0",
        "onnx>=1.11.0",
        "onnxmltools==1.10.0",
        "torch>=1.12.1",
        "pyyaml>=5.4",
        "typeguard>=2.3.13",
        "packaging>=20.9",
        # Necessary until upstream packages account for the breaking
        # change to numpy
        "numpy<2.0.0",
        "pandas>=1.5.3",
        "fasteners",
        "GitPython>=3.1.40",
        "psutil",
        "wmi",
        "pytz",
        "tqdm",
        "zstandard",
        "matplotlib",
        "tabulate",
        # Conditional dependencies for ONNXRuntime backends
        "onnxruntime >=1.10.1;platform_system=='Linux' and extra != 'llm-oga-cuda'",
        "onnxruntime-directml >=1.19.0;platform_system=='Windows' and extra != 'llm-oga-cuda'",
        "onnxruntime-gpu >=1.19.1;extra == 'llm-oga-cuda'",
    ],
    extras_require={
        "llm": [
            "torch>=2.0.0",
            "transformers",
            "accelerate",
            "py-cpuinfo",
            "sentencepiece",
            "datasets",
            # Install human-eval from a forked repo with Windows support until the
            # PR (https://github.com/openai/human-eval/pull/53) is merged
            "human-eval-windows==1.0.4",
            "fastapi",
            "uvicorn[standard]",
            "openai",
        ],
        "llm-oga-cpu": [
            "onnxruntime-genai>=0.5.2",
            "torch>=2.0.0,<2.4",
            "transformers<4.45.0",
            "turnkeyml[llm]",
        ],
        "llm-oga-igpu": [
            "onnxruntime-genai-directml>=0.4.0",
            "torch>=2.0.0,<2.4",
            "transformers<4.45.0",
            "turnkeyml[llm]",
        ],
        "llm-oga-cuda": [
            "onnxruntime-genai-cuda>=0.4.0",
            "torch>=2.0.0,<2.4",
            "transformers<4.45.0",
            "turnkeyml[llm]",
        ],
        "llm-oga-npu": [
            "onnx==1.16.0",
            "onnxruntime==1.18.0",
            "numpy==1.26.4",
            "turnkeyml[llm]",
        ],
        "llm-oga-hybrid": [
            "onnx==1.16.1",
            "numpy==1.26.4",
            "turnkeyml[llm]",
        ],
    },
    classifiers=[],
    entry_points={
        "console_scripts": [
            "turnkey=turnkeyml:turnkeycli",
            "turnkey-llm=lemonade:lemonadecli",
            "lemonade=lemonade:lemonadecli",
            "lemonade-install=lemonade_install:installcli",
        ]
    },
    python_requires=">=3.8, <3.12",
    long_description=open("README.md", "r", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    include_package_data=True,
    package_data={
        "turnkeyml_models": ["requirements.txt", "readme.md"],
    },
)
