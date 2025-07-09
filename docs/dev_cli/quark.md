# Quick Guide to Quark Quantization Tools

## Introduction
Quark is indeed AMD's recommended quantization framework for targeting Ryzen AI platforms, supporting both PyTorch and ONNX formats. For Quark specific info, please visit [quark-doc](https://quark.docs.amd.com/latest/). Here's a guide on using Quark tools for quantization and reloading a quantized model using lemonade:

## Installation

1. Create and activate a conda environment:
    - `conda create -n quark python=3.10`
    - `conda activate quark`
2. Install requirements to setup this environment.
Depending on your usecase you can install for CPU, NPU pr hybrid. 
    ```bash
    pip install -e .[dev,oga-cpu] # Can also work with llm-oga-npu or llm-oga-hybrid
    ```
2. Install `quark` using `lemonade-install` for easy install
    ```bash
    # Install the latest external version of quark
    lemonade-install --quark 0.6.0
    ```
    This downloads the .whl files and zip folder from the Quark page, installs, and sets up the environment for Quark.

## Usage
```bash
lemonade -i <model-ckpt> huggingface-load quark-quantize 
    --model-export <export_format> # Export formats [quark_safetensors, onnx, gguf]
    --quant-algo <quantization_algorithm> # Supported algorithms [gptq, awq, autosmoothquant] 
    --quant-scheme <quantization_scheme> # Quant schemes [w_int4, w_uint4, w_int8...] 
    --device <device> # Target device [cpu, cuda] 
    llm-prompt -p "<prompt>"
```
## Example Workflows
### Quantize and Export

This command quantizes an opt-125m loaded from HF, using AWQ qunatization algorithm to generate A8W8 quantized model. Running quantization on CPU can be time consuming. This test can take upto 1hr using 
100% of your CPU.

```bash
lemonade -i facebook/opt-125m huggingface-load quark-quantize --quant-algo awq --quant-scheme w_int8_a_int8_per_tensor_sym --model-export quark_safetensors --device cpu
```

#### Load Quantized Model:
This command loads the exported model from a cache folder that corresponds to the quantization recipe used during its export.
```bash
lemonade -i facebook/opt-125m huggingface-load quark-load --safetensors-model-reload --quant-algo awq --quant-scheme w_int8_a_int8_per_tensor_sym --device cpu llm-prompt -p "Hello world"
```

### Supported Quantization Schemes

The following are the different quantization schemes supported for various models.
For a comprehensive list of datatype support for specific models, refer to the [support matrix](https://quark.docs.amd.com/latest/pytorch/example_quark_torch_llm_ptq.html#id11).

- w_uint4_per_group_asym
- w_int4_per_channel_sym
- w_int8_a_int8_per_tensor_sym
- w_int8_per_tensor_sym and more..

For more information on the supported quantization schemes, see [Language Model Post Training Quantization (PTQ) Using Quark](https://quark.docs.amd.com/latest/pytorch/example_quark_torch_llm_ptq.html).

### Supported Export Formats

Lemonade supports exporting quark quantized models in various formats. The following export formats are available:

- quark_safetensors
- onnx
- vllm_adopted_safetensors
- gguf

## Known Issues
- No PyPI installer for Quark yet. You can use lemondade-installer as mentioned [above](#installation) for Quark installation.
- Not enough Quark APIs are exposed. Need to rely heavily of Zip folder released by Quark. 
- Latest Quark version is hardcoded in quark_quantize for download checks.

- There is currently no PyPI installer for Quark. You can use lemonade-installer as mentioned in the [Installation Section](#installation) of this guide for Quark installation.
- There are limited Quark APIs currently available. Users will need to rely on the Zip folder released by Quark. 
- Latest Quark version hardcoded in quark_quantize for download checks.
- Unable to suppress logging info from Quark. Using log_severity_level, you can suppress the quantization logs, but you cannot suppress info and warning messages when reloading the model, etc.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->