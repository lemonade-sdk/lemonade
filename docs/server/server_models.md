
# 🍋 Lemonade Server Models
 
This document provides the models we recommend for use with Lemonade Server. Click on any model to learn more details about it, such as the [Lemonade Recipe](https://github.com/lemonade-sdk/lemonade/blob/main/docs/lemonade_api.md) used to load the model.

## Naming Convention

The format of each Lemonade name is a combination of the name in the base checkpoint and the backend where the model will run. So, if the base checkpoint is `meta-llama/Llama-3.2-1B-Instruct`, and it has been optimized to run on Hybrid, the resulting name is `Llama-3.2-3B-Instruct-Hybrid`.

## Model Storage and Management

Lemonade Server relies on [Hugging Face Hub](https://huggingface.co/docs/hub/en/index) to manage downloading and storing models on your system. By default, Hugging Face Hub downloads models to `C:\Users\YOUR_USERNAME\.cache\huggingface\hub`.

For example, the Lemonade Server `Llama-3.2-3B-Instruct-Hybrid` model will end up at `C:\Users\YOUR_USERNAME\.cache\huggingface\hub\models--amd--Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid`. If you want to uninstall that model, simply delete that folder.

You can change the directory for Hugging Face Hub by [setting the `HF_HOME` or `HF_HUB_CACHE` environment variables](https://huggingface.co/docs/huggingface_hub/en/package_reference/environment_variables).

## Installing Additional Models

Once you've installed Lemonade Server, you can install any model on this list using the `pull` command in the [`lemonade-server` CLI](./lemonade-server-cli.md).

Example:

```bash
lemonade-server pull Qwen2.5-0.5B-Instruct-CPU
```

> Note: `lemonade-server` is a utility that is added to your PATH when you install Lemonade Server with the GUI installer.
> If you are using Lemonade Server from a Python environment, use the `lemonade-server-dev pull` command instead.

## Supported Models

### Hybrid

<details>
<summary>Llama-3.2-1B-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Llama-3.2-1B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid">amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-3B-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Llama-3.2-3B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.2-3B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid">amd/Llama-3.2-3B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Phi-3-Mini-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Phi-3-Mini-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Phi-3-mini-4k-instruct-awq-g128-int4-asym-fp16-onnx-hybrid">amd/Phi-3-mini-4k-instruct-awq-g128-int4-asym-fp16-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Qwen-1.5-7B-Chat-Hybrid</summary>

```bash
    lemonade-server pull Qwen-1.5-7B-Chat-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen1.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid">amd/Qwen1.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-R1-Distill-Llama-8B-Hybrid</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Llama-8B-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-hybrid">amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-R1-Distill-Qwen-7B-Hybrid</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Qwen-7B-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/DeepSeek-R1-Distill-Qwen-7B-awq-asym-uint4-g128-lmhead-onnx-hybrid">amd/DeepSeek-R1-Distill-Qwen-7B-awq-asym-uint4-g128-lmhead-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>Mistral-7B-v0.3-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Mistral-7B-v0.3-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Mistral-7B-Instruct-v0.3-awq-g128-int4-asym-fp16-onnx-hybrid">amd/Mistral-7B-Instruct-v0.3-awq-g128-int4-asym-fp16-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Llama-3.1-8B-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Llama-3.1-8B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.1-8B-Instruct-awq-asym-uint4-g128-lmhead-onnx-hybrid">amd/Llama-3.1-8B-Instruct-awq-asym-uint4-g128-lmhead-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>


### CPU

<details>
<summary>Qwen2.5-0.5B-Instruct-CPU</summary>

```bash
    lemonade-server pull Qwen2.5-0.5B-Instruct-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx">amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-1B-Instruct-CPU</summary>

```bash
    lemonade-server pull Llama-3.2-1B-Instruct-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.2-1B-Instruct-awq-uint4-float16-cpu-onnx">amd/Llama-3.2-1B-Instruct-awq-uint4-float16-cpu-onnx</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-3B-Instruct-CPU</summary>

```bash
    lemonade-server pull Llama-3.2-3B-Instruct-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.2-3B-Instruct-awq-uint4-float16-cpu-onnx">amd/Llama-3.2-3B-Instruct-awq-uint4-float16-cpu-onnx</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Phi-3-Mini-Instruct-CPU</summary>

```bash
    lemonade-server pull Phi-3-Mini-Instruct-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Phi-3-mini-4k-instruct_int4_float16_onnx_cpu">amd/Phi-3-mini-4k-instruct_int4_float16_onnx_cpu</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>Qwen-1.5-7B-Chat-CPU</summary>

```bash
    lemonade-server pull Qwen-1.5-7B-Chat-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen1.5-7B-Chat_uint4_asym_g128_float16_onnx_cpu">amd/Qwen1.5-7B-Chat_uint4_asym_g128_float16_onnx_cpu</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>False</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-R1-Distill-Llama-8B-CPU</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Llama-8B-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu">amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-R1-Distill-Qwen-7B-CPU</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Qwen-7B-CPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu">amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu</a></td></tr>
<tr><td>Recipe</td><td>oga-cpu</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>


### GGUF

<details>
<summary>Qwen3-0.6B-GGUF</summary>

```bash
    lemonade-server pull Qwen3-0.6B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-0.6B-GGUF:Q4_0">unsloth/Qwen3-0.6B-GGUF:Q4_0</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>Qwen3-1.7B-GGUF</summary>

```bash
    lemonade-server pull Qwen3-1.7B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-1.7B-GGUF:Q4_0">unsloth/Qwen3-1.7B-GGUF:Q4_0</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-GGUF</summary>

```bash
    lemonade-server pull Qwen3-4B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-4B-GGUF:Q4_0">unsloth/Qwen3-4B-GGUF:Q4_0</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>Qwen3-8B-GGUF</summary>

```bash
    lemonade-server pull Qwen3-8B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-8B-GGUF:Q4_0">unsloth/Qwen3-8B-GGUF:Q4_0</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>Qwen3-14B-GGUF</summary>

```bash
    lemonade-server pull Qwen3-14B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-14B-GGUF:Q4_0">unsloth/Qwen3-14B-GGUF:Q4_0</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>

<details>
<summary>Qwen3-30B-A3B-GGUF</summary>

```bash
    lemonade-server pull Qwen3-30B-A3B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-30B-A3B-GGUF:Q4_0">unsloth/Qwen3-30B-A3B-GGUF:Q4_0</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Reasoning</td><td>True</td></tr>
</table>

</details>


<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->