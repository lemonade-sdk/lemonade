# 🍋 Lemonade SDK

Welcome to the documentation for the Lemonade SDK project! Use this resource to learn more about the server, CLI, API, and how to contribute to the project.

<div class="hide-in-mkdocs">

- [Installation](#installation)
- [Server](#server)
- [Developer CLI](#developer-cli)
- [Lemonade API](#lemonade-api)
- [Software and Hardware Overview](#software-and-hardware-overview)
  - [Supported Hardware Accelerators](#supported-hardware-accelerators)
  - [Supported Inference Engines](#supported-inference-engines)
- [Contributing](#contributing)
</div>

## Installation


[Click here for Lemonade SDK installation options](https://lemonade-server.ai/install_options.html).

For a quick start with Hugging Face (PyTorch) LLMs on CPU, run the following installation commands in an active Python 3 environment, and then try the Server, CLI, or API links below.

```bash
pip install lemonade-sdk[dev]
```

## Server

The Lemonade Server is an OpenAI API-compatible HTTP server that supports streamlined integration with a wide variety of LLM applications. Learn more in [server documentation](https://lemonade-server.ai/docs/).

## Developer CLI

The Lemonade developer CLI, `lemonade`, offers tools for performance benchmarking, accuracy evaluation, and device-specific model preparation. Learn more in the dev CLI [README.md](./dev_cli/README.md).

## Lemonade API

The high-level Lemonade API abstracts loading models from any supported framework (e.g., Hugging Face, OGA) and backend (e.g., CPU, Hybrid) using the popular `from_pretrained()` function. This makes it easy to integrate Lemonade LLMs into Python applications. For more information on recipes and compatibility, see the [Lemonade API ReadMe](./lemonade_api.md).

OGA Hybrid:
```python
from lemonade.api import from_pretrained

model, tokenizer = from_pretrained("amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid", recipe="oga-hybrid")

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))
```

You can find examples for the high-level APIs [here](https://github.com/lemonade-sdk/lemonade/tree/main/examples).

## Software and Hardware Overview

The goal of Lemonade is to help achieve maximum LLM performance on your PC. To cover a wide range of PCs, Lemonade supports a wide variety of hardware accelerators and inference engines described in the subsections below.

### Supported Hardware Accelerators

| Mode | Description |
| :--- | :--- |
| **NPU & Hybrid** | Ryzen™ AI 300-series devices have a neural processing unit (NPU) that can run LLMs and accelerate time-to-first-token (TTFT) performance. The typical way of utilizing the NPU is called *hybrid execution*, where the prompt is processed on the NPU to produce the first token, and the remaining tokens are computed on the Ryzen AI integrated GPU (iGPU). |
| **GPU** | PCs with an integrated GPU (iGPU), such as many laptop SoCs, and/or discrete GPU (dGPU), such as many desktop and workstation PCs, can run LLMs on that GPU hardware. Lemonade Server provides GPU support in every installation via the Vulkan llama.cpp binaries.<br/><br/> <sub>Note: GPU support is not currently provided for CLI tasks such as benchmarking.</sub> |

### Supported Inference Engines
| Engine | Description |
| :--- | :--- |
| **OnnxRuntime GenAI (OGA)** | Microsoft engine that runs `.onnx` models and enables hardware vendors to provide their own execution providers (EPs) to support specialized hardware, such as neural processing units (NPUs). |
| **llamacpp** | Community-driven engine with strong GPU acceleration, support for thousands of `.gguf` models, and advanced features such as vision-language models (VLMs) and mixture-of-experts (MoEs). |
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
