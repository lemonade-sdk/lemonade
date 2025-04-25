# 🍋 Lemonade SDK

*The long-term objective of the Lemonade SDK is to provide the ONNX ecosystem with the same kind of tools available in the GGUF ecosystem.*

Lemonade SDK is built on top of [OnnxRuntime GenAI (OGA)](https://github.com/microsoft/onnxruntime-genai), an ONNX LLM inference engine developed by Microsoft to improve the LLM experience on AI PCs, especially those with accelerator hardware such as Neural Processing Units (NPUs).

The Lemonade SDK provides everything needed to get up and running quickly with LLMs on OGA:

| **Feature**                              | **Description**                                                                                     |
|------------------------------------------|-----------------------------------------------------------------------------------------------------|
| **🌐 Local LLM server with OpenAI API compatibility (Lemonade Server)** | Replace cloud-based LLMs with private and free LLMs that run locally on your own PC's NPU and GPU. |
| **🖥️ CLI with tools for prompting, benchmarking, and accuracy tests**  | Enables convenient interoperability between models, frameworks, devices, accuracy tests, and deployment options. |
| **🐍 Python API based on `from_pretrained()`**                          | Provides easy integration with Python applications for loading and using LLMs.                      |


## Table of Contents

- [Installation](#installation)
  - [Installing Lemonade Server via Executable](#installing-from-lemonade_server_installerexe)
  - [Installing Lemonade SDK From PyPI](#installing-from-pypi)
  - [Installing Lemonade SDK From Source](#installing-from-source)
- [CLI Commands](#cli-commands)
  - [Prompting](#prompting)
  - [Accuracy](#accuracy)
  - [Benchmarking](#benchmarking)
  - [LLM Report](#llm-report)
  - [Memory Usage](#memory-usage)
  - [Serving](#serving)
- [API](#api)
  - [High-Level APIs](#high-level-apis)
  - [Low-Level API](#low-level-api)
- [Contributing](#contributing)


# Installation

There are 3 ways a user can install the Lemonade SDK:

1. Use the [Lemonade Server Installer](#installing-from-lemonade_server_installerexe). This provides a no code way to run LLMs locally and integrate with OpenAI compatible applications.
1. Use [PyPI installation](#installing-from-pypi) by installing the `turnkeyml` package with the appropriate extras for your backend. This will install the full set of Turnkey and Lemonade SDK tools, including Lemonade Server, API, and CLI commands.
1. Use [source installation](#installing-from-source) if you plan to contribute or customize the Lemonade SDK.


## Installing From Lemonade_Server_Installer.exe

The Lemonade Server is available as a standalone tool with a one-click Windows installer `.exe`. Check out the [Lemonade_Server_Installer.exe guide](lemonade_server_exe.md) for installation instructions and the [server spec](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_spec.md) to learn more about the functionality.

The Lemonade Server [examples folder](https://github.com/onnx/turnkeyml/tree/main/examples/lemonade/server) has guides for how to use Lemonade Server with a collection of applications that we have tested.

## Installing From PyPI

To install the Lemonade SDK from PyPI:

1. Create and activate a [miniconda](https://repo.anaconda.com/miniconda/Miniconda3-latest-Windows-x86_64.exe) environment.
    ```bash
    conda create -n lemon python=3.10
    ```

    ```bash
    conda activate lemon
    ```

3. Install Lemonade for your backend of choice: 
    - [OnnxRuntime GenAI with CPU backend](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/ort_genai_igpu.md): 
        ```bash
        pip install turnkeyml[llm-oga-cpu]
        ```
    - [OnnxRuntime GenAI with Integrated GPU (iGPU, DirectML) backend](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/ort_genai_igpu.md):
        > Note: Requires Windows and a DirectML-compatible iGPU.
        ```bash
        pip install turnkeyml[llm-oga-igpu]
        ```
    - OnnxRuntime GenAI with Ryzen AI Hybrid (NPU + iGPU) backend:
        > Note: Ryzen AI Hybrid requires a Windows 11 PC with an AMD Ryzen™ AI 300-series processor.

        - Follow the environment setup instructions [here](https://ryzenai.docs.amd.com/en/latest/llm/high_level_python.html)
    - Hugging Face (PyTorch) LLMs for CPU backend:
        ```bash
            pip install turnkeyml[llm]
        ```
    - llama.cpp: see [instructions](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/llamacpp.md).

4. Use `lemonade -h` to explore the LLM tools, and see the [command](#cli-commands) and [API](#api) examples below.

## Installing From Source

The Lemonade SDK can be installed from source code by cloning this repository and following the instructions [here](source_installation_inst.md).


# CLI Commands

The `lemonade` CLI uses a unique command syntax that enables convenient interoperability between models, frameworks, devices, accuracy tests, and deployment options.

Each unit of functionality (e.g., loading a model, running a test, deploying a server, etc.) is called a `Tool`, and a single call to `lemonade` can invoke any number of `Tools`. Each `Tool` will perform its functionality, then pass its state to the next `Tool` in the command.

You can read each command out loud to understand what it is doing. For example, a command like this:

```bash
lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 llm-prompt -p "Hello, my thoughts are"
```

Can be read like this:

> Run `lemonade` on the input `(-i)` checkpoint `microsoft/Phi-3-mini-4k-instruct`. First, load it in the OnnxRuntime GenAI framework (`oga-load`), onto the integrated GPU device (`--device igpu`) in the int4 data type (`--dtype int4`). Then, pass the OGA model to the prompting tool (`llm-prompt`) with the prompt (`-p`) "Hello, my thoughts are" and print the response.

The `lemonade -h` command will show you which options and Tools are available, and `lemonade TOOL -h` will tell you more about that specific Tool.


## Prompting

To prompt your LLM, try one of the following:

OGA iGPU:
```bash
    lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 llm-prompt -p "Hello, my thoughts are" -t
```

Hugging Face:
```bash
    lemonade -i facebook/opt-125m huggingface-load llm-prompt -p "Hello, my thoughts are" -t
```

The LLM will run with your provided prompt, and the LLM's response to your prompt will be printed to the screen. You can replace the `"Hello, my thoughts are"` with any prompt you like.

You can also replace the `facebook/opt-125m` with any Hugging Face checkpoint you like, including LLaMA-2, Phi-2, Qwen, Mamba, etc.

You can also set the `--device` argument in `oga-load` and `huggingface-load` to load your LLM on a different device.

The `-t` (or `--template`) flag instructs lemonade to insert the prompt string into the model's chat template.
This typically results in the model returning a higher quality response.

Run `lemonade huggingface-load -h` and `lemonade llm-prompt -h` to learn more about these tools.

## Accuracy

To measure the accuracy of an LLM using MMLU (Measuring Massive Multitask Language Understanding), try the following:

OGA iGPU:
```bash
    lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 accuracy-mmlu --tests management
```

Hugging Face:
```bash
    lemonade -i facebook/opt-125m huggingface-load accuracy-mmlu --tests management
```

This command will run just the management test from MMLU on your LLM and save the score to the lemonade cache at `~/.cache/lemonade`. You can also run other subject tests by replacing management with the new test subject name. For the full list of supported subjects, see the [MMLU Accuracy Read Me](mmlu_accuracy.md).

You can run the full suite of MMLU subjects by omitting the `--test` argument. You can learn more about this with `lemonade accuracy-mmlu -h`.

## Benchmarking

To measure the time-to-first-token and tokens/second of an LLM, try the following:

OGA iGPU:
```bash
    lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 oga-bench
```

Hugging Face:
```bash
    lemonade -i facebook/opt-125m huggingface-load huggingface-bench
```

This command will run a few warm-up iterations, then a few generation iterations where performance data is collected.

The prompt size, number of output tokens, and number iterations are all parameters. Learn more by running `lemonade oga-bench -h` or `lemonade huggingface-bench -h`.

## LLM Report

To see a report that contains all the benchmarking results and all the accuracy results, use the `report` tool with the `--perf` flag:

`lemonade report --perf`

The results can be filtered by model name, device type and data type.  See how by running `lemonade report -h`.

## Memory Usage

The peak memory used by the `lemonade` build is captured in the build output. To capture more granular
memory usage information, use the `--memory` flag.  For example:

OGA iGPU:
```bash
    lemonade --memory -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 oga-bench
```

Hugging Face:
```bash
    lemonade --memory -i facebook/opt-125m huggingface-load huggingface-bench
```

In this case a `memory_usage.png` file will be generated and stored in the build folder.  This file
contains a figure plotting the memory usage over the build time.  Learn more by running `lemonade -h`.

## Serving

You can launch an OpenAI-compatible server with:

```bash
    lemonade serve
```

Visit the [server spec](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_spec.md) to learn more about the endpoints provided as well as how to launch the server with more detailed informational messages enabled.

See the Lemonade Server [examples folder](https://github.com/onnx/turnkeyml/tree/main/examples/lemonade/server) to see a collection of applications that we have tested with Lemonade Server.

# API

Lemonade is also available via API.

## High-Level APIs

The high-level Lemonade API abstracts loading models from any supported framework (e.g., Hugging Face, OGA) and backend (e.g., CPU, iGPU, Hybrid) using the popular `from_pretrained()` function. This makes it easy to integrate Lemonade LLMs into Python applications. For more information on recipes and compatibility, see the [Lemonade API ReadMe](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/lemonade_api.md).

OGA iGPU:
```python
from lemonade.api import from_pretrained

model, tokenizer = from_pretrained("Qwen/Qwen2.5-0.5B-Instruct", recipe="oga-igpu")

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))
```

You can learn more about the high-level APIs [here](https://github.com/onnx/turnkeyml/tree/main/examples/lemonade).

## Low-Level API

The low-level API is useful for designing custom experiments. For example, sweeping over specific checkpoints, devices, and/or tools.

Here's a quick example of how to prompt a Hugging Face LLM using the low-level API, which calls the load and prompt tools one by one:

```python
import lemonade.tools.torch_llm as tl
import lemonade.tools.prompt as pt
from turnkeyml.state import State

state = State(cache_dir="cache", build_name="test")

state = tl.HuggingfaceLoad().run(state, input="facebook/opt-125m")
state = pt.Prompt().run(state, prompt="hi", max_new_tokens=15)

print("Response:", state.response)
```

# Contributing

Contributions are welcome! If you decide to contribute, please:

- Do so via a pull request.
- Write your code in keeping with the same style as the rest of this repo's code.
- Add a test under `test/lemonade` that provides coverage of your new feature.

The best way to contribute is to add new tools to cover more devices and usage scenarios.

To add a new tool:

1. (Optional) Create a new `.py` file under `src/lemonade/tools` (or use an existing file if your tool fits into a pre-existing family of tools).
1. Define a new class that inherits the `Tool` class from `TurnkeyML`.
1. Register the class by adding it to the list of `tools` near the top of `src/lemonade/cli.py`.

You can learn more about contributing on the repository's [contribution guide](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md).
