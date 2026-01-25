# lemonade-eval CLI

Contents:

- [Overview](#overview)
- [Installation](#installation)
- [Available Tools](#available-tools)
- [Server-Based Workflow](#server-based-workflow)
- [NPU and Hybrid Models](#npu-and-hybrid-models)
- [OGA-Load for Model Preparation](#oga-load-for-model-preparation)
- [Accuracy Testing](#accuracy-testing)
- [Benchmarking](#benchmarking)
- [Export a Finetuned Model](#exporting-a-finetuned-model)
- [LLM Report](#llm-report)
- [Memory Usage](#memory-usage)
- [System Information](#system-information)

## Overview

The `lemonade-eval` CLI provides tools for evaluating, benchmarking, and preparing LLMs. It is designed to work alongside the [Lemonade Server](../server/README.md), enabling:

- **Performance benchmarking** of models running on Lemonade Server
- **Accuracy testing** using MMLU, HumanEval, Perplexity, and lm-eval-harness
- **Model preparation** for OGA (ONNX Runtime GenAI) on NPU and CPU devices

The CLI uses a unique command syntax where each unit of functionality is called a `Tool`. A single call to `lemonade-eval` can invoke multiple `Tools` in sequence, with each tool passing its state to the next.

For example:

```bash
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load bench
```

Can be read as:

> Run `lemonade-eval` on the input `(-i)` model `Qwen3-4B-Instruct-2507-GGUF`. First, load it on the Lemonade Server (`load`), then benchmark it (`bench`).

Use `lemonade-eval -h` to see available options and tools, and `lemonade-eval TOOL -h` for help on a specific tool.

## Installation

### 1. Install Lemonade Server

Install from the [latest release](https://github.com/lemonade-sdk/lemonade/releases/latest):
- **Windows**: Download and run `lemonade-server.msi`
- **Linux**: See [Linux installation options](https://lemonade-server.ai/install_options.html#linux)

### 2. Create a Python Environment

Choose one of the following methods:

**Using venv:**

```bash
python -m venv lemon
# Windows:
lemon\Scripts\activate
# Linux/macOS:
source lemon/bin/activate
```

**Using conda:**

```bash
conda create -n lemon python=3.12
conda activate lemon
```

**Using uv:**

```bash
uv venv lemon --python 3.12
# Windows:
lemon\Scripts\activate
# Linux/macOS:
source lemon/bin/activate
```

### 3. Install lemonade-eval

Basic installation:

```bash
pip install lemonade-sdk
```

**Optional extras:**

```bash
# For OGA CPU inference:
pip install lemonade-sdk[oga-cpu]

# For RyzenAI NPU support (Windows + Python 3.12 only):
pip install lemonade-sdk[oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple

# For model generation/export (Windows + Python 3.12 only):
pip install lemonade-sdk[oga-ryzenai,model-generate] --extra-index-url=https://pypi.amd.com/simple
```

## Available Tools

| Tool | Description |
|------|-------------|
| `load` | Load a model onto a running Lemonade Server |
| `bench` | Benchmark a model loaded on Lemonade Server |
| `oga-load` | Load and prepare OGA models for NPU/CPU inference |
| `accuracy-mmlu` | Evaluate accuracy using MMLU benchmark |
| `accuracy-humaneval` | Evaluate code generation accuracy |
| `accuracy-perplexity` | Calculate perplexity scores |
| `lm-eval-harness` | Run lm-evaluation-harness benchmarks |
| `llm-prompt` | Send a prompt to a loaded model |
| `report` | Display benchmarking and accuracy results |
| `cache` | Manage the lemonade-eval cache |
| `version` | Display version information |
| `system-info` | Query system information from Lemonade Server |

## Server-Based Workflow

Most `lemonade-eval` tools require a running Lemonade Server. Start the server first:

```bash
lemonade-server serve
```

Then use `lemonade-eval` to load models and run evaluations:

```bash
# Load a model and prompt it
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load llm-prompt -p "Hello, world!"

# Load and benchmark a model
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load bench

# Load and run accuracy tests
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load accuracy-mmlu --tests management
```

### Server Connection Options

By default, `lemonade-eval` connects to `http://localhost:8000`. Use `--server-url` to connect to a different server:

```bash
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load --server-url http://192.168.1.100:8000 bench
```

## NPU and Hybrid Models

For NPU and Hybrid inference on AMD Ryzen AI processors, use Lemonade Server with `-NPU` or `-Hybrid` models:

```bash
# Load and prompt a Hybrid model (NPU + iGPU)
lemonade-eval -i Llama-3.2-1B-Instruct-Hybrid load llm-prompt -p "Hello!"

# Load and benchmark an NPU model
lemonade-eval -i Qwen-2.5-3B-Instruct-NPU load bench

# Load and run accuracy tests on Hybrid
lemonade-eval -i Qwen3-4B-Hybrid load accuracy-mmlu --tests management
```

### Requirements for NPU/Hybrid

- **Processor**: AMD Ryzen AI 300- and 400-series processors (e.g., Strix Point, Krackan Point, Gorgon Point)
- **Operating System**: Windows 11
- **NPU Driver**: Install the [NPU Driver](https://ryzenai.docs.amd.com/en/latest/inst.html#install-npu-drivers)

See the [Models List](https://lemonade-server.ai/docs/server/server_models/) for all available `-NPU` and `-Hybrid` models.

## OGA-Load for Model Preparation

The `oga-load` tool is for preparing custom OGA (ONNX Runtime GenAI) models. It can build and quantize models from Hugging Face for use on NPU, iGPU, or CPU.

> **Note**: For running pre-built NPU/Hybrid models, use the server-based workflow above with `-NPU` or `-Hybrid` models. The `oga-load` tool is primarily for model preparation and testing custom checkpoints.

### Usage

```bash
# Prepare and test a model on CPU
lemonade-eval -i microsoft/Phi-3-mini-4k-instruct oga-load --device cpu --dtype int4 llm-prompt -p "Hello!"
```

### Installation for OGA

```bash
pip install lemonade-sdk[oga-cpu]
# OR for RyzenAI NPU support:
pip install lemonade-sdk[oga-ryzenai] --extra-index-url=https://pypi.amd.com/simple
```

See [OGA for iGPU and CPU](ort_genai_igpu.md) for more details on model building and caching.

## Accuracy Testing

### MMLU

Test language understanding across many subjects:

```bash
# With GGUF model
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load accuracy-mmlu --tests management

# With Hybrid model
lemonade-eval -i Qwen3-4B-Hybrid load accuracy-mmlu --tests management
```

See [MMLU Accuracy](mmlu_accuracy.md) for the full list of subjects.

### HumanEval

Test code generation capabilities:

```bash
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load accuracy-humaneval
```

See [HumanEval Accuracy](humaneval_accuracy.md) for details.

### Perplexity

Calculate perplexity scores (requires OGA model loaded via `oga-load`):

```bash
lemonade-eval -i microsoft/Phi-3-mini-4k-instruct oga-load --device cpu --dtype int4 accuracy-perplexity
```

See [Perplexity Evaluation](perplexity.md) for interpretation guidance.

### lm-eval-harness

Run standardized benchmarks from lm-evaluation-harness:

```bash
# Run GSM8K math benchmark with GGUF model
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load lm-eval-harness --task gsm8k --limit 10

# Run with Hybrid model
lemonade-eval -i Qwen3-4B-Hybrid load lm-eval-harness --task gsm8k --limit 10
```

See [lm-eval-harness](lm-eval.md) for supported tasks and options.

## Benchmarking

### With Lemonade Server

Benchmark models loaded on Lemonade Server:

```bash
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load bench
```

The benchmark measures:
- **Time to First Token (TTFT)**: Latency before first token is generated
- **Tokens per Second**: Generation throughput
- **Memory Usage**: Peak memory consumption (with `--memory` flag)

#### Options

```bash
lemonade-eval -i Qwen3-4B-Instruct-2507-GGUF load bench --iterations 5 --warmup-iterations 2 --output-tokens 128
```

## Exporting a Finetuned Model

To prepare your own fine-tuned model for OGA:

1. Quantize the model using Quark
2. Export using `oga-load`

See the [Finetuned Model Export Guide](finetuned_model_export.md) for detailed instructions.

## LLM Report

View a summary of all benchmarking and accuracy results:

```bash
lemonade-eval report --perf
```

Results can be filtered by model name, device type, and data type:

```bash
lemonade-eval report --perf --filter-model "Qwen"
```


## Power Profiling

For power profiling, see [Power Profiling](power_profiling.md).

## System Information

To view system information and available devices, use the `system-info` tool:

```bash
lemonade-eval system-info
```

By default, this shows essential information including OS version, processor, and physical memory.

For detailed system information including BIOS version, CPU max clock, Windows power setting, and Python packages, use the `--verbose` flag:

```bash
lemonade-eval system-info --verbose
```

For JSON output format:

```bash
lemonade-eval system-info --format json
```

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
