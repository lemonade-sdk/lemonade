
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

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid](https://huggingface.co/amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | False |

</details>

<details>
<summary>Llama-3.2-3B-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Llama-3.2-3B-Instruct-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Llama-3.2-3B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid](https://huggingface.co/amd/Llama-3.2-3B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | False |

</details>

<details>
<summary>Phi-3-Mini-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Phi-3-Mini-Instruct-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Phi-3-mini-4k-instruct-awq-g128-int4-asym-fp16-onnx-hybrid](https://huggingface.co/amd/Phi-3-mini-4k-instruct-awq-g128-int4-asym-fp16-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | False |

</details>

<details>
<summary>Qwen-1.5-7B-Chat-Hybrid</summary>

```bash
    lemonade-server pull Qwen-1.5-7B-Chat-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Qwen1.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid](https://huggingface.co/amd/Qwen1.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | False |

</details>

<details>
<summary>DeepSeek-R1-Distill-Llama-8B-Hybrid</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Llama-8B-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-hybrid](https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | True |

</details>

<details>
<summary>DeepSeek-R1-Distill-Qwen-7B-Hybrid</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Qwen-7B-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/DeepSeek-R1-Distill-Qwen-7B-awq-asym-uint4-g128-lmhead-onnx-hybrid](https://huggingface.co/amd/DeepSeek-R1-Distill-Qwen-7B-awq-asym-uint4-g128-lmhead-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | True |

</details>

<details>
<summary>Mistral-7B-v0.3-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Mistral-7B-v0.3-Instruct-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Mistral-7B-Instruct-v0.3-awq-g128-int4-asym-fp16-onnx-hybrid](https://huggingface.co/amd/Mistral-7B-Instruct-v0.3-awq-g128-int4-asym-fp16-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | False |

</details>

<details>
<summary>Llama-3.1-8B-Instruct-Hybrid</summary>

```bash
    lemonade-server pull Llama-3.1-8B-Instruct-Hybrid
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Llama-3.1-8B-Instruct-awq-asym-uint4-g128-lmhead-onnx-hybrid](https://huggingface.co/amd/Llama-3.1-8B-Instruct-awq-asym-uint4-g128-lmhead-onnx-hybrid) |
| Recipe | oga-hybrid |
| Reasoning | False |

</details>


### CPU

<details>
<summary>Qwen2.5-0.5B-Instruct-CPU</summary>

```bash
    lemonade-server pull Qwen2.5-0.5B-Instruct-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx](https://huggingface.co/amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx) |
| Recipe | oga-cpu |
| Reasoning | False |

</details>

<details>
<summary>Llama-3.2-1B-Instruct-CPU</summary>

```bash
    lemonade-server pull Llama-3.2-1B-Instruct-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Llama-3.2-1B-Instruct-awq-uint4-float16-cpu-onnx](https://huggingface.co/amd/Llama-3.2-1B-Instruct-awq-uint4-float16-cpu-onnx) |
| Recipe | oga-cpu |
| Reasoning | False |

</details>

<details>
<summary>Llama-3.2-3B-Instruct-CPU</summary>

```bash
    lemonade-server pull Llama-3.2-3B-Instruct-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Llama-3.2-3B-Instruct-awq-uint4-float16-cpu-onnx](https://huggingface.co/amd/Llama-3.2-3B-Instruct-awq-uint4-float16-cpu-onnx) |
| Recipe | oga-cpu |
| Reasoning | False |

</details>

<details>
<summary>Phi-3-Mini-Instruct-CPU</summary>

```bash
    lemonade-server pull Phi-3-Mini-Instruct-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Phi-3-mini-4k-instruct_int4_float16_onnx_cpu](https://huggingface.co/amd/Phi-3-mini-4k-instruct_int4_float16_onnx_cpu) |
| Recipe | oga-cpu |
| Reasoning | False |

</details>

<details>
<summary>Qwen-1.5-7B-Chat-CPU</summary>

```bash
    lemonade-server pull Qwen-1.5-7B-Chat-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/Qwen1.5-7B-Chat_uint4_asym_g128_float16_onnx_cpu](https://huggingface.co/amd/Qwen1.5-7B-Chat_uint4_asym_g128_float16_onnx_cpu) |
| Recipe | oga-cpu |
| Reasoning | False |

</details>

<details>
<summary>DeepSeek-R1-Distill-Llama-8B-CPU</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Llama-8B-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu](https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu) |
| Recipe | oga-cpu |
| Reasoning | True |

</details>

<details>
<summary>DeepSeek-R1-Distill-Qwen-7B-CPU</summary>

```bash
    lemonade-server pull DeepSeek-R1-Distill-Qwen-7B-CPU
```

| Key | Value |
| --- | ----- |
| Checkpoint | [amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu](https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-awq-asym-uint4-g128-lmhead-onnx-cpu) |
| Recipe | oga-cpu |
| Reasoning | True |

</details>


<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->