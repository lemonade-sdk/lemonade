# 🍋 Lemonade SDK

<div class="hide-in-mkdocs">

- [Installation](#installation)
  - [Hybrid Execution](#hybrid-execution)
  - [GPU Execution](#gpu-execution)
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
</div>

## Installation


[Click here for Lemonade SDK installation options](https://lemonade-server.ai/install_options.html).

For a quick start with Hugging Face (PyTorch) LLMs on CPU, run the following installation commands in an active Python 3 environment, and then try one of the CLI commands below.

```bash
pip install lemonade-sdk[llm]
```

### NPU and Hybrid Execution

Ryzen™ AI 300-series devices have a neural processing unit (NPU) that can run LLMs and accelerate time-to-first-token (TTFT) performance.

The typical way of utilizing the NPU is called *hybrid execution*, where the prompt is processed on the NPU to produce the first token, and the remaining tokens are computed on the Ryzen AI integrated GPU (iGPU).

### GPU Execution

PCs with an integrated GPU (iGPU), such as many laptop SoCs, and/or discrete GPU (dGPU), such as many desktop and workstation PCs, can run LLMs on that GPU hardware. Lemonade Server provides GPU support in every installation via the Vulkan llama.cpp binaries. 

> Note: GPU support is not currently provided for CLI tasks such as benchmarking.

## CLI Commands

The `lemonade` CLI uses a unique command syntax that enables convenient interoperability between models, frameworks, devices, accuracy tests, and deployment options.

Each unit of functionality (e.g., loading a model, running a test, deploying a server, etc.) is called a `Tool`, and a single call to `lemonade` can invoke any number of `Tools`. Each `Tool` will perform its functionality, then pass its state to the next `Tool` in the command.

You can read each command out loud to understand what it is doing. For example, a command like this:

```bash
lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid --device hybrid --dtype int4 llm-prompt -p "Hello, my thoughts are"
```

Can be read like this:

> Run `lemonade` on the input `(-i)` checkpoint `amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid` (which is meta-llama/Llama-3.2-1B-Instruct optimized for OGA and hybrid). First, load it in the OnnxRuntime GenAI framework (`oga-load`), onto hybrid NPU/GPU acceleration (`--device hybrid`) in the int4 data type (`--dtype int4`). Then, pass the OGA model to the prompting tool (`llm-prompt`) with the prompt (`-p`) "Hello, my thoughts are" and print the response.

The `lemonade -h` command will show you which options and Tools are available, and `lemonade TOOL -h` will tell you more about that specific Tool.


### Prompting

To prompt your LLM, try one of the following:

OGA Hybrid:
```bash
    lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid --device hybrid --dtype int4 llm-prompt -p "Hello, my thoughts are" -t
```

Hugging Face:
```bash
    lemonade -i facebook/opt-125m huggingface-load llm-prompt -p "Hello, my thoughts are" -t
```

The LLM will run with your provided prompt, and the LLM's response to your prompt will be printed to the screen. You can replace the `"Hello, my thoughts are"` with any prompt you like.

You can also replace the `facebook/opt-125m` with any Hugging Face checkpoint you like, including LLaMA, Phi, Qwen, Mamba, etc.

You can also set the `--device` argument in `oga-load` and `huggingface-load` to load your LLM on a different device.

The `-t` (or `--template`) flag instructs Lemonade to insert the prompt string into the model's chat template.
This typically results in the model returning a higher quality response.

Run `lemonade huggingface-load -h` and `lemonade llm-prompt -h` to learn more about these tools.

### Accuracy

To measure the accuracy of an LLM using MMLU (Measuring Massive Multitask Language Understanding), try the following:

OGA Hybrid:
```bash
    lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid --device hybrid --dtype int4 accuracy-mmlu --tests management
```

Hugging Face:
```bash
    lemonade -i facebook/opt-125m huggingface-load accuracy-mmlu --tests management
```

This command will run just the management test from MMLU on your LLM and save the score to the Lemonade cache at `~/.cache/lemonade`. You can also run other subject tests by replacing management with the new test subject name. For the full list of supported subjects, see the [MMLU Accuracy Read Me](mmlu_accuracy.md).

You can run the full suite of MMLU subjects by omitting the `--test` argument. You can learn more about this with `lemonade accuracy-mmlu -h`.

### Benchmarking

To measure the time-to-first-token and tokens/second of an LLM, try the following:

OGA Hybrid:
```bash
    lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid --device hybrid --dtype int4 oga-bench
```

Hugging Face:
```bash
    lemonade -i facebook/opt-125m huggingface-load huggingface-bench
```

This command will run a few warm-up iterations, then a few generation iterations where performance data is collected.

The prompt size, number of output tokens, and number iterations are all parameters. Learn more by running `lemonade oga-bench -h` or `lemonade huggingface-bench -h`.

### LLM Report

To see a report that contains all the benchmarking results and all the accuracy results, use the `report` tool with the `--perf` flag:
```bash
    lemonade report --perf
```

The results can be filtered by model name, device type and data type.  See how by running `lemonade report -h`.

### Memory Usage

The peak memory used by the Lemonade execution sequence is captured in the build output. To capture more granular
memory usage information, use the `--memory` flag.  For example:

OGA Hybrid:
```bash
    lemonade --memory -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid --device hybrid --dtype int4 oga-bench
```

Hugging Face:
```bash
    lemonade --memory -i facebook/opt-125m huggingface-load huggingface-bench
```

This generates a PNG file that is stored in the current folder and the build folder.  This file
contains a figure plotting the memory usage over the Lemonade tool sequence.  Learn more by running `lemonade -h`.

## API

Lemonade is also available via API.

### High-Level APIs

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

### Low-Level API

The low-level API is useful for designing custom experiments. For example, sweeping over specific checkpoints, devices, and/or tools.

Here's a quick example of how to prompt a Hugging Face LLM using the low-level API, which calls the load and prompt tools one by one:

```python
import lemonade.tools.torch_llm as tl
import lemonade.tools.prompt as pt
from lemonade.state import State

state = State(cache_dir="cache", build_name="test")

state = tl.HuggingfaceLoad().run(state, input="facebook/opt-125m")
state = pt.Prompt().run(state, prompt="hi", max_new_tokens=15)

print("Response:", state.response)
```

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
