# Turnkey-LLM

Welcome to the project page for `turnkey-llm` (aka, "lemonade" the turnkey LLM Aide)!
Contents:

1. [Getting Started](#getting-started)
1. [Install Specialized Tools](#install-specialized-tools)
1. [Code Organization](#code-organization)
1. [Contributing](#contributing)

# Getting Started

`turnkey-llm` introduces a brand new set of LLM-focused tools. 

## Install

1. Clone: `git clone https://github.com/onnx/turnkeyml.git`
1. `cd turnkeyml` (where `turnkeyml` is the repo root of your TurnkeyML clone)
    - Note: be sure to run these installation instructions from the repo root.
1. Create and activate a conda environment:
    1. `conda create -n tk-llm python=3.10`
    1. `conda activate tk-llm`
1. Install lemonade: `pip install -e .[llm]`
    - or `pip install -e .[llm-oga-dml]` if you want to use `onnxruntime-genai` (see [OGA](#install-onnxruntime-genai))
1. `lemonade -h` to explore the LLM tools

## Syntax

The `lemonade` CLI uses the same style of syntax as `turnkey`, but with a new set of LLM-specific tools. You can read about that syntax [here](https://github.com/onnx/turnkeyml#how-it-works).

## Chatting

To chat with your LLM try:

`lemonade -i facebook/opt-125m huggingface-load llm-prompt -p "Hello, my thoughts are"`

The LLM will run on CPU with your provided prompt, and the LLM's response to your prompt will be printed to the screen. You can replace the `"Hello, my thoughts are"` with any prompt you like.

You can also replace the `facebook/opt-125m` with any Huggingface checkpoint you like, including LLaMA-2, Phi-2, Qwen, Mamba, etc.

You can also set the `--device` argument in `huggingface-load` to load your LLM on a different device.

Run `lemonade huggingface-load -h` and `lemonade llm-prompt -h` to learn more about those tools.

## Accuracy

To measure the accuracy of an LLM using MMLU, try this:

`lemonade -i facebook/opt-125m huggingface-load accuracy-mmlu --tests management`

That command will run just the management test from MMLU on your LLM and save the score to the lemonade cache at `~/.cache/lemonade`.

You can run the full suite of MMLU subjects by omitting the `--test` argument. You can learn more about this with `lemonade accuracy-mmlu -h.

## Serving

You can launch a WebSocket server for your LLM with:

`lemonade -i facebook/opt-125m huggingface-load serve`

Once the server has launched, you can connect to it from your own application, or interact directly by following the on-screen instructions to open a basic web app.

Note that the `llm-prompt`, `accuracy-mmlu`, and `serve` tools can all be used with other model-loading tools, for example `onnxruntime-genai` or `ryzenai-transformers`. See [Install Specialized Tools](#install-specialized-tools) for details.

## API

Lemonade is also available via API. Here's a quick example of how to benchmark an LLM:

```python
import turnkeyml.llm.tools.torch_llm as tl
import turnkeyml.llm.tools.chat as cl
from turnkeyml.state import State

state = State(cache_dir="cache", build_name="test")

state = tl.HuggingfaceLoad().run(state, input="facebook/opt-125m")
state = cl.Prompt().run(state, prompt="hi", max_new_tokens=15)

print("Response:", state.response)
```

# Install Specialized Tools

Lemonade supports specialized tools that each require their own setup steps. **Note:** These tools will only appear in `lemonade -h` if you run in an environment that has completed setup.

## Install OnnxRuntime-GenAI

To install support for [onnxruntime-genai](https://github.com/microsoft/onnxruntime-genai) (e.g., the `oga-load` Tool), use `pip install -e .[llm-oga-dml]` instead of the default installation command.

Next, you need to get an OGA model. Per the OGA instructions, we suggest Phi-3-Mini. Use the following command to download it from Hugging Face, and make sure to set your `--local-dir` to the `REPO_ROOT/src/turnkeyml/llm/ort_genai/models` directory.

`huggingface-cli download microsoft/Phi-3-mini-4k-instruct-onnx --include directml/directml-int4-awq-block-128* --local-dir REPO_ROOT/src/turnkeyml/llm/tools/ort_genai/models/phi-3-mini-4k-instruct`

You can try it out with: `lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 llm-prompt -p "Hello, my thoughts are"`

You can also try Phi-3-Mini-128k-Instruct with the following commands:

`huggingface-cli download microsoft/Phi-3-mini-128k-instruct-onnx --include directml/directml-int4-awq-block-128* --local-dir REPO_ROOT/src/turnkeyml/llm/tools/ort_genai/models/phi-3-mini-128k-instruct`

`lemonade -i microsoft/Phi-3-mini-128k-instruct oga-load --device igpu --dtype int4 llm-prompt -p "Hello, my thoughts are"`


> Note: no other models or devices are officially supported by `lemonade` on OGA at this time. Contributions appreciated!

## Install Ryzen AI NPU

To run your LLMs on Ryzen AI NPU, first install and set up the `ryzenai-transformers` conda environment (see instructions [here](https://github.com/amd/RyzenAI-SW/tree/main/example/transformers)). Then, install `lemonade` into `ryzenai-transformers`. The `ryzenai-npu-load` Tool will become available in that environment.

You can try it out with: `lemonade -i meta-llama/Llama-2-7b-chat-hf ryzenai-npu-load --device DEVICE llm-prompt -p "Hello, my thoughts are"`

Where `DEVICE` is either "phx" or "stx" if you have a RyzenAI 7xxx/8xxx or 3xx/9xxx processor, respectively.

> Note: only `meta-llama/Llama-2-7b-chat-hf` and `microsoft/Phi-3-mini-4k-instruct` are supported by `lemonade` at this time. Contributions appreciated!

# Contributing

If you decide to contribute, please:

- do so via a pull request.
- write your code in keeping with the same style as the rest of this repo's code.
- add a test under `test/llm_api.py` that provides coverage of your new feature.

The best way to contribute is to add new tools to cover more devices and usage scenarios.

To add a new tool:

1. (Optional) Create a new `.py` file under `src/turnkeyml/llm/tools` (or use an existing file if your tool fits into a pre-existing family of tools).
1. Define a new class that inherits the `Tool` class from `TurnkeyML`.
1. Register the class by adding it to the list of `tools` near the top of `src/turnkeyml/llm/cli.py`.
