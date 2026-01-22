# 🍋 Lemonade SDK

Welcome to the documentation for the Lemonade SDK project! Use this resource to learn more about the server, CLI, and how to contribute to the project.

<div class="hide-in-mkdocs">

- [Installation](#installation)
- [Server](#server)
- [lemonade-eval](#lemonade-eval)
- [Software and Hardware Overview](#software-and-hardware-overview)
  - [Supported Hardware Accelerators](#supported-hardware-accelerators)
  - [Supported Inference Engines](#supported-inference-engines)
- [Contributing](#contributing)
</div>

## Installation


[Click here for Lemonade SDK installation options](https://lemonade-server.ai/install_options.html).

For a quick start, run the following installation commands in an active Python 3 environment, and then try the Server or CLI links below.

```bash
pip install lemonade-sdk[dev]
```

## Server

The Lemonade Server is an OpenAI API-compatible HTTP server that supports streamlined integration with a wide variety of LLM applications. Learn more in [server documentation](https://lemonade-server.ai/docs/).

## lemonade-eval

The `lemonade-eval` CLI offers tools for performance benchmarking, accuracy evaluation, and device-specific model preparation. Learn more in the [lemonade-eval README](./dev_cli/README.md).

## Software and Hardware Overview

The goal of Lemonade is to help achieve maximum LLM performance on your PC. To cover a wide range of PCs, Lemonade supports a wide variety of hardware accelerators and inference engines described in the subsections below.

### Supported Hardware Accelerators

| Mode | Description |
| :--- | :--- |
| **NPU & Hybrid** | Ryzen™ AI 300-series devices have a neural processing unit (NPU) that can run LLMs and accelerate time-to-first-token (TTFT) performance. The typical way of utilizing the NPU is called *hybrid execution*, where the prompt is processed on the NPU to produce the first token, and the remaining tokens are computed on the Ryzen AI integrated GPU (iGPU). |
| **GPU** | PCs with an integrated GPU (iGPU), such as many laptop SoCs, and/or discrete GPU (dGPU), such as many desktop and workstation PCs, can run LLMs on that GPU hardware. Lemonade Server provides GPU support via Vulkan llama.cpp binaries (Windows, Linux, macOS), ROCm binaries (Windows, Linux), and Metal binaries (macOS with Apple Silicon).<br/><br/> <sub>Note: GPU support is not currently provided for CLI tasks such as benchmarking.</sub> |

### Supported Inference Engines
| Engine | Description |
| :--- | :--- |
| **OnnxRuntime GenAI (OGA)** | Microsoft engine that runs `.onnx` models and enables hardware vendors to provide their own execution providers (EPs) to support specialized hardware, such as neural processing units (NPUs). |
| **llamacpp** | Community-driven engine with strong GPU acceleration, support for thousands of `.gguf` models, and advanced features such as vision-language models (VLMs) and mixture-of-experts (MoEs). |
| **FastFlowLM** | A [startup-driven engine](https://github.com/FastFlowLM/FastFlowLM) optimized for Ryzen™ AI 300-series NPUs, with support for `.q4nx` models with vision-language model (VLM) support. Available in early access; commercial licensing terms apply. |
| **Hugging Face (HF)** | Hugging Face's `transformers` library can run the original `.safetensors` trained weights for models on Meta's PyTorch engine, which provides a source of truth for accuracy measurement. |

## Contributing

Contributions are welcome! If you decide to contribute, please:

- Do so via a pull request.
- Write your code in keeping with the same style as the rest of this repo's code.
- Add a test under `test/` that provides coverage of your new feature.

The best way to contribute is to add new tools to cover more devices and usage scenarios.

To add a new tool:

1. (Optional) Create a new `.py` file under `src/lemonade/tools` (or use an existing file if your tool fits into a pre-existing family of tools).
1. Define a new class that inherits the `Tool` class.
1. Register the class by adding it to the list of `tools` near the top of `src/lemonade/cli.py`.

You can learn more about contributing on the repository's [contribution guide](https://github.com/lemonade-sdk/lemonade/blob/main/docs/contribute.md).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->