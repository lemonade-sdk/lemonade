# 🍋 Lemonade API: Model Compatibility and Recipes

Lemonade API (`lemonade.api`) provides a simple, high-level interface to load and run LLM models locally. This guide helps you understand what models work with which **recipes**, what to expect in terms of compatibility, and how to choose the right setup for your hardware.

## 🧠 What Is a Recipe?

A **recipe** defines how a model is run — including backend (e.g., PyTorch, ONNX Runtime), quantization strategy, and device support. The `from_pretrained()` function in `lemonade.api` uses the recipe to configure everything automatically. For the list of recipes, see [Recipe Compatibility Table](#recipe-and-checkpoint-compatibility). The following is an example of using the Lemonade API `from_pretrained()` function:

```python
from lemonade.api import from_pretrained

model, tokenizer = from_pretrained("Qwen/Qwen2.5-0.5B-Instruct", recipe="hf-cpu")
```

Function Arguments:
- checkpoint: The Hugging Face or OGA checkpoint that defines the LLM.
- recipe: Defines the implementation and hardware used for the LLM. Default is "hf-cpu".

## 🔍 System Information Functions

The Lemonade API also provides functions to programmatically access system and device information:

```python
from lemonade.api import get_system_info, get_device_info

# Get essential system information (OS, processor, memory, devices)
system_info = get_system_info()

# Get detailed system information including Python packages and extended details
system_info_verbose = get_system_info(verbose=True)

# Get only device information (CPU, GPU, NPU details)
device_info = get_device_info()
```

These functions return the same information available through the `lemonade system-info` CLI command and the `/api/v1/system-info` server endpoint, including:
- Hardware details (CPU, memory, BIOS)
- Device availability (AMD iGPU, dGPU, NPU)
- Inference engine compatibility per device
- Driver versions and system configuration
- Python package versions (verbose mode only)


## 📜 Supported Model Formats

Lemonade API currently supports:

- Hugging Face hosted **safetensors** checkpoints
- AMD **OGA** (ONNXRuntime-GenAI) ONNX checkpoints

<a name="recipe-and-checkpoint-compatibility"></a>
## 🍴 Recipe and Checkpoint Compatibility

The following table explains what checkpoints work with each recipe, the hardware and OS requirements, and additional notes:

<table>
  <tr>
    <th>Recipe</th>
    <th>Checkpoint Format</th>
    <th>Hardware Needed</th>
    <th>Operating System</th>
    <th>Notes</th>
  </tr>
  <tr>
    <td><code>hf-cpu</code></td>
    <td>safetensors (Hugging Face)</td>
    <td>Any x86 CPU</td>
    <td>Windows, Linux</td>
    <td>Compatible with x86 CPUs, offering broad accessibility.</td>
  </tr>
  <tr>
    <td><code>hf-dgpu</code></td>
    <td>safetensors (Hugging Face)</td>
    <td>Compatible Discrete GPU</td>
    <td>Windows, Linux</td>
    <td>Requires PyTorch and a compatible GPU.<sup>[1]</sup></td>
  </tr>
  <tr>
    <td rowspan="2"><code>oga-cpu</code></td>
    <td>safetensors (Hugging Face)</td>
    <td>Any x86 CPU</td>
    <td>Windows</td>
    <td>Converted from safetensors via `model_builder`. Accuracy loss due to RTN quantization.</td>
  </tr>
  <tr>
    <td>OGA ONNX</td>
    <td>Any x86 CPU</td>
    <td>Windows</td>
    <td>Use models from the <a href="https://huggingface.co/collections/amd/oga-cpu-llm-collection-6808280dc18d268d57353be8">CPU Collection.</a></td>
  </tr>
  <tr>
    <td>OGA ONNX</td>
    <td>AMD Ryzen AI PC</td>
    <td>Windows</td>
    <td>Use models from the <a href="https://huggingface.co/collections/amd/ryzenai-oga-dml-models-67f940914eee51cbd794b95b">GPU Collection.</a></td>
  </tr>
  <tr>
    <td><code>oga-hybrid</code></td>
    <td>Pre-quantized OGA ONNX</td>
    <td>AMD Ryzen AI 300 series PC</td>
    <td>Windows</td>
    <td>Use models from the <a href="https://huggingface.co/collections/amd/ryzenai-14-llm-hybrid-models-67da31231bba0f733750a99c">Hybrid Collection</a>. Optimized with AWQ to INT4.</td>
  </tr>
  <tr>
    <td><code>oga-npu</code></td>
    <td>Pre-quantized OGA ONNX</td>
    <td>AMD Ryzen AI 300 series PC</td>
    <td>Windows</td>
    <td>Use models from the <a href="https://huggingface.co/collections/amd/ryzenai-14-llm-npu-models-67da3494ec327bd3aa3c83d7">NPU Collection</a>. Optimized with AWQ to INT4.</td>
  </tr>
</table>

<sup>[1]</sup> Compatible GPUs are those that support PyTorch's `.to("cuda")` function. Ensure you have the appropriate version of PyTorch installed (e.g., CUDA or ROCm) for your specific GPU. **Note**: Lemonade does not install PyTorch with CUDA or ROCm for you. For installation instructions, see [PyTorch's Get Started Guide](https://pytorch.org/get-started/locally/).

## 🔄 Converting Models to OGA

Lemonade API will do the conversion for you using OGA's `model_builder` if you pass a safetensors checkpoint.

- Takes \~1–5 minutes per model.
- Uses RTN quantization (int4).
- For better quality, use pre-quantized models (see below).


## 📦 Pre-Converted OGA Models

You can skip the conversion step by using pre-quantized models from AMD’s Hugging Face collection. These models are optimized using **Activation Aware Quantization (AWQ)**, which provides higher-accuracy int4 quantization compared to RTN.

| Recipe       | Collection                                                                                                                                      |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `oga-hybrid` | [Hybrid Collection](https://huggingface.co/collections/amd/ryzenai-14-llm-hybrid-models-67da31231bba0f733750a99c)                               |
| `oga-npu`    | [NPU Collection](https://huggingface.co/collections/amd/ryzenai-14-llm-npu-models-67da3494ec327bd3aa3c83d7)                                     |
| `oga-cpu`    | [CPU Collection](https://huggingface.co/collections/amd/oga-cpu-llm-collection-6808280dc18d268d57353be8) |



## 📚 Additional Resources

- [Lemonade API Examples](https://github.com/lemonade-sdk/lemonade/blob/main/examples#api-examples)
- [lemonade.api source](https://github.com/lemonade-sdk/lemonade/blob/main/src/lemonade/api.py)
- [Model Support Matrix (ONNX Runtime GenAI)](https://github.com/microsoft/onnxruntime-genai)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->