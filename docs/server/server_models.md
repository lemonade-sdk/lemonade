
# 🍋 Lemonade Server Models
 
This document provides the models we recommend for use with Lemonade Server.

Click on any model to learn more details about it, such as the [Lemonade Recipe](https://github.com/lemonade-sdk/lemonade/blob/main/docs/lemonade_api.md) used to load the model. Content:

- [Model Management GUI](#model-management-gui)
- [Supported Models](#supported-models)
- [Naming Convention](#naming-convention)
- [Model Storage and Management](#model-storage-and-management)
- [Installing Additional Models](#installing-additional-models)

## Model Management GUI

Lemonade Server offers a model management GUI to help you see which models are available, install new models, and delete models. You can access this GUI by starting Lemonade Server, opening http://localhost:8000 in your web browser, and clicking the Model Management tab.

## Supported Models

### 🔥 Hot Models

<details>
<summary>Qwen3-4B-Instruct-2507-GGUF</summary>

```bash
lemonade-server pull Qwen3-4B-Instruct-2507-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF">unsloth/Qwen3-4B-Instruct-2507-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-4B-Instruct-2507-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot</td></tr>
<tr><td>Size (GB)</td><td>2.5</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Coder-30B-A3B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen3-Coder-30B-A3B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF">unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>coding, tool-calling, hot</td></tr>
<tr><td>Size (GB)</td><td>18.6</td></tr>
</table>

</details>

<details>
<summary>Gemma-3-4b-it-GGUF</summary>

```bash
lemonade-server pull Gemma-3-4b-it-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF">ggml-org/gemma-3-4b-it-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Mmproj</td><td>mmproj-model-f16.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot, vision</td></tr>
<tr><td>Size (GB)</td><td>3.61</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Next-80B-A3B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen3-Next-80B-A3B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-Next-80B-A3B-Instruct-GGUF">unsloth/Qwen3-Next-80B-A3B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Next-80B-A3B-Instruct-UD-Q4_K_XL.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot</td></tr>
<tr><td>Size (GB)</td><td>45.1</td></tr>
</table>

</details>

<details>
<summary>gpt-oss-120b-mxfp-GGUF</summary>

```bash
lemonade-server pull gpt-oss-120b-mxfp-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/gpt-oss-120b-GGUF">ggml-org/gpt-oss-120b-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>*</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot, reasoning, tool-calling</td></tr>
<tr><td>Size (GB)</td><td>63.3</td></tr>
</table>

</details>

<details>
<summary>gpt-oss-20b-mxfp4-GGUF</summary>

```bash
lemonade-server pull gpt-oss-20b-mxfp4-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/gpt-oss-20b-GGUF">ggml-org/gpt-oss-20b-GGUF</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot, reasoning, tool-calling</td></tr>
<tr><td>Size (GB)</td><td>12.1</td></tr>
</table>

</details>

<details>
<summary>Gemma3-4b-it-FLM</summary>

```bash
lemonade-server pull Gemma3-4b-it-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>gemma3:4b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>hot, vision</td></tr>
<tr><td>Size (GB)</td><td>5.26</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-VL-FLM</summary>

```bash
lemonade-server pull Qwen3-4B-VL-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>qwen3vl-it:4b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>hot, vision</td></tr>
<tr><td>Size (GB)</td><td>3.85</td></tr>
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
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-0.6B-GGUF">unsloth/Qwen3-0.6B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_0</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>0.38</td></tr>
</table>

</details>

<details>
<summary>Qwen3-1.7B-GGUF</summary>

```bash
lemonade-server pull Qwen3-1.7B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-1.7B-GGUF">unsloth/Qwen3-1.7B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_0</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>1.06</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-GGUF</summary>

```bash
lemonade-server pull Qwen3-4B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-4B-GGUF">unsloth/Qwen3-4B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_0</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>2.38</td></tr>
</table>

</details>

<details>
<summary>Qwen3-8B-GGUF</summary>

```bash
lemonade-server pull Qwen3-8B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-8B-GGUF">unsloth/Qwen3-8B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_1</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>5.25</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-Qwen3-8B-GGUF</summary>

```bash
lemonade-server pull DeepSeek-Qwen3-8B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/DeepSeek-R1-0528-Qwen3-8B-GGUF">unsloth/DeepSeek-R1-0528-Qwen3-8B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_1</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>5.25</td></tr>
</table>

</details>

<details>
<summary>Qwen3-14B-GGUF</summary>

```bash
lemonade-server pull Qwen3-14B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-14B-GGUF">unsloth/Qwen3-14B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_0</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>8.54</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-Instruct-2507-GGUF</summary>

```bash
lemonade-server pull Qwen3-4B-Instruct-2507-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF">unsloth/Qwen3-4B-Instruct-2507-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-4B-Instruct-2507-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot</td></tr>
<tr><td>Size (GB)</td><td>2.5</td></tr>
</table>

</details>

<details>
<summary>Qwen3-30B-A3B-GGUF</summary>

```bash
lemonade-server pull Qwen3-30B-A3B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-30B-A3B-GGUF">unsloth/Qwen3-30B-A3B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_0</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>17.4</td></tr>
</table>

</details>

<details>
<summary>Qwen3-30B-A3B-Instruct-2507-GGUF</summary>

```bash
lemonade-server pull Qwen3-30B-A3B-Instruct-2507-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF">unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-30B-A3B-Instruct-2507-Q4_0.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>17.4</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Coder-30B-A3B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen3-Coder-30B-A3B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF">unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>coding, tool-calling, hot</td></tr>
<tr><td>Size (GB)</td><td>18.6</td></tr>
</table>

</details>

<details>
<summary>Gemma-3-4b-it-GGUF</summary>

```bash
lemonade-server pull Gemma-3-4b-it-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF">ggml-org/gemma-3-4b-it-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Mmproj</td><td>mmproj-model-f16.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot, vision</td></tr>
<tr><td>Size (GB)</td><td>3.61</td></tr>
</table>

</details>

<details>
<summary>Phi-4-mini-instruct-GGUF</summary>

```bash
lemonade-server pull Phi-4-mini-instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Phi-4-mini-instruct-GGUF">unsloth/Phi-4-mini-instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Phi-4-mini-instruct-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>2.49</td></tr>
</table>

</details>

<details>
<summary>LFM2-1.2B-GGUF</summary>

```bash
lemonade-server pull LFM2-1.2B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/LiquidAI/LFM2-1.2B-GGUF">LiquidAI/LFM2-1.2B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>LFM2-1.2B-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>0.731</td></tr>
</table>

</details>

<details>
<summary>Jan-nano-128k-GGUF</summary>

```bash
lemonade-server pull Jan-nano-128k-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Menlo/Jan-nano-128k-gguf">Menlo/Jan-nano-128k-gguf</a></td></tr>
<tr><td>GGUF Variant</td><td>jan-nano-128k-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>2.5</td></tr>
</table>

</details>

<details>
<summary>Jan-v1-4B-GGUF</summary>

```bash
lemonade-server pull Jan-v1-4B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/janhq/Jan-v1-4B-GGUF">janhq/Jan-v1-4B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Jan-v1-4B-Q4_K_M.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>2.5</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-1B-Instruct-GGUF</summary>

```bash
lemonade-server pull Llama-3.2-1B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Llama-3.2-1B-Instruct-GGUF">unsloth/Llama-3.2-1B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Llama-3.2-1B-Instruct-UD-Q4_K_XL.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>0.834</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-3B-Instruct-GGUF</summary>

```bash
lemonade-server pull Llama-3.2-3B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Llama-3.2-3B-Instruct-GGUF">unsloth/Llama-3.2-3B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Llama-3.2-3B-Instruct-UD-Q4_K_XL.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>2.06</td></tr>
</table>

</details>

<details>
<summary>SmolLM3-3B-GGUF</summary>

```bash
lemonade-server pull SmolLM3-3B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/SmolLM3-3B-128K-GGUF">unsloth/SmolLM3-3B-128K-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>SmolLM3-3B-128K-UD-Q4_K_XL.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>1.94</td></tr>
</table>

</details>

<details>
<summary>Ministral-3-3B-Instruct-2512-GGUF</summary>

```bash
lemonade-server pull Ministral-3-3B-Instruct-2512-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/mistralai/Ministral-3-3B-Instruct-2512-GGUF">mistralai/Ministral-3-3B-Instruct-2512-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Ministral-3-3B-Instruct-2512-Q4_K_M.gguf</td></tr>
<tr><td>Mmproj</td><td>Ministral-3-3B-Instruct-2512-BF16-mmproj.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>vision</td></tr>
<tr><td>Size (GB)</td><td>2.85</td></tr>
</table>

</details>

<details>
<summary>Qwen2.5-VL-7B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen2.5-VL-7B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/Qwen2.5-VL-7B-Instruct-GGUF">ggml-org/Qwen2.5-VL-7B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Mmproj</td><td>mmproj-Qwen2.5-VL-7B-Instruct-f16.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>vision</td></tr>
<tr><td>Size (GB)</td><td>4.68</td></tr>
</table>

</details>

<details>
<summary>Qwen3-VL-4B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen3-VL-4B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct-GGUF">Qwen/Qwen3-VL-4B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Mmproj</td><td>mmproj-Qwen3VL-4B-Instruct-F16.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>vision</td></tr>
<tr><td>Size (GB)</td><td>3.33</td></tr>
</table>

</details>

<details>
<summary>Qwen3-VL-8B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen3-VL-8B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct-GGUF">Qwen/Qwen3-VL-8B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Mmproj</td><td>mmproj-Qwen3VL-8B-Instruct-F16.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>vision</td></tr>
<tr><td>Size (GB)</td><td>6.19</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Next-80B-A3B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen3-Next-80B-A3B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Qwen3-Next-80B-A3B-Instruct-GGUF">unsloth/Qwen3-Next-80B-A3B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Next-80B-A3B-Instruct-UD-Q4_K_XL.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot</td></tr>
<tr><td>Size (GB)</td><td>45.1</td></tr>
</table>

</details>

<details>
<summary>Llama-4-Scout-17B-16E-Instruct-GGUF</summary>

```bash
lemonade-server pull Llama-4-Scout-17B-16E-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/Llama-4-Scout-17B-16E-Instruct-GGUF">unsloth/Llama-4-Scout-17B-16E-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_S</td></tr>
<tr><td>Mmproj</td><td>mmproj-F16.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>vision</td></tr>
<tr><td>Size (GB)</td><td>61.5</td></tr>
</table>

</details>

<details>
<summary>nomic-embed-text-v1-GGUF</summary>

```bash
lemonade-server pull nomic-embed-text-v1-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/nomic-ai/nomic-embed-text-v1-GGUF">nomic-ai/nomic-embed-text-v1-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_S</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>embeddings</td></tr>
<tr><td>Size (GB)</td><td>0.0781</td></tr>
</table>

</details>

<details>
<summary>nomic-embed-text-v2-moe-GGUF</summary>

```bash
lemonade-server pull nomic-embed-text-v2-moe-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/nomic-ai/nomic-embed-text-v2-moe-GGUF">nomic-ai/nomic-embed-text-v2-moe-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q8_0</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>embeddings</td></tr>
<tr><td>Size (GB)</td><td>0.51</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Embedding-0.6B-GGUF</summary>

```bash
lemonade-server pull Qwen3-Embedding-0.6B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF">Qwen/Qwen3-Embedding-0.6B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Embedding-0.6B-Q8_0.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>embeddings</td></tr>
<tr><td>Size (GB)</td><td>0.64</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Embedding-4B-GGUF</summary>

```bash
lemonade-server pull Qwen3-Embedding-4B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Qwen/Qwen3-Embedding-4B-GGUF">Qwen/Qwen3-Embedding-4B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Embedding-4B-Q8_0.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>embeddings</td></tr>
<tr><td>Size (GB)</td><td>4.28</td></tr>
</table>

</details>

<details>
<summary>Qwen3-Embedding-8B-GGUF</summary>

```bash
lemonade-server pull Qwen3-Embedding-8B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Qwen/Qwen3-Embedding-8B-GGUF">Qwen/Qwen3-Embedding-8B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Qwen3-Embedding-8B-Q8_0.gguf</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>embeddings</td></tr>
<tr><td>Size (GB)</td><td>8.05</td></tr>
</table>

</details>

<details>
<summary>bge-reranker-v2-m3-GGUF</summary>

```bash
lemonade-server pull bge-reranker-v2-m3-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/pqnet/bge-reranker-v2-m3-Q8_0-GGUF">pqnet/bge-reranker-v2-m3-Q8_0-GGUF</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reranking</td></tr>
<tr><td>Size (GB)</td><td>0.53</td></tr>
</table>

</details>

<details>
<summary>Devstral-Small-2507-GGUF</summary>

```bash
lemonade-server pull Devstral-Small-2507-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/mistralai/Devstral-Small-2507_gguf">mistralai/Devstral-Small-2507_gguf</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>coding, tool-calling</td></tr>
<tr><td>Size (GB)</td><td>14.3</td></tr>
</table>

</details>

<details>
<summary>Qwen2.5-Coder-32B-Instruct-GGUF</summary>

```bash
lemonade-server pull Qwen2.5-Coder-32B-Instruct-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/Qwen/Qwen2.5-Coder-32B-Instruct-GGUF">Qwen/Qwen2.5-Coder-32B-Instruct-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>coding</td></tr>
<tr><td>Size (GB)</td><td>19.85</td></tr>
</table>

</details>

<details>
<summary>gpt-oss-120b-mxfp-GGUF</summary>

```bash
lemonade-server pull gpt-oss-120b-mxfp-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/gpt-oss-120b-GGUF">ggml-org/gpt-oss-120b-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>*</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot, reasoning, tool-calling</td></tr>
<tr><td>Size (GB)</td><td>63.3</td></tr>
</table>

</details>

<details>
<summary>gpt-oss-20b-mxfp4-GGUF</summary>

```bash
lemonade-server pull gpt-oss-20b-mxfp4-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/ggml-org/gpt-oss-20b-GGUF">ggml-org/gpt-oss-20b-GGUF</a></td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>hot, reasoning, tool-calling</td></tr>
<tr><td>Size (GB)</td><td>12.1</td></tr>
</table>

</details>

<details>
<summary>GLM-4.5-Air-UD-Q4K-XL-GGUF</summary>

```bash
lemonade-server pull GLM-4.5-Air-UD-Q4K-XL-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/GLM-4.5-Air-GGUF">unsloth/GLM-4.5-Air-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>UD-Q4_K_XL</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>73.1</td></tr>
</table>

</details>

<details>
<summary>granite-4.0-h-tiny-GGUF</summary>

```bash
lemonade-server pull granite-4.0-h-tiny-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/unsloth/granite-4.0-h-tiny-GGUF">unsloth/granite-4.0-h-tiny-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Labels</td><td>tool-calling</td></tr>
<tr><td>Size (GB)</td><td>4.25</td></tr>
</table>

</details>

<details>
<summary>LFM2-8B-A1B-GGUF</summary>

```bash
lemonade-server pull LFM2-8B-A1B-GGUF
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/LiquidAI/LFM2-8B-A1B-GGUF">LiquidAI/LFM2-8B-A1B-GGUF</a></td></tr>
<tr><td>GGUF Variant</td><td>Q4_K_M</td></tr>
<tr><td>Recipe</td><td>llamacpp</td></tr>
<tr><td>Size (GB)</td><td>4.8</td></tr>
</table>

</details>


### Ryzen AI Hybrid (NPU+GPU)

<details>
<summary>Llama-3.2-1B-Instruct-Hybrid</summary>

```bash
lemonade-server pull Llama-3.2-1B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.2-1B-Instruct-onnx-ryzenai-hybrid">amd/Llama-3.2-1B-Instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>1.89</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-3B-Instruct-Hybrid</summary>

```bash
lemonade-server pull Llama-3.2-3B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Llama-3.2-3B-Instruct-onnx-ryzenai-hybrid">amd/Llama-3.2-3B-Instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>4.28</td></tr>
</table>

</details>

<details>
<summary>Phi-3-Mini-Instruct-Hybrid</summary>

```bash
lemonade-server pull Phi-3-Mini-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Phi-3-mini-4k-instruct-onnx-ryzenai-hybrid">amd/Phi-3-mini-4k-instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>4.18</td></tr>
</table>

</details>

<details>
<summary>Qwen-1.5-7B-Chat-Hybrid</summary>

```bash
lemonade-server pull Qwen-1.5-7B-Chat-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen1.5-7B-Chat-onnx-ryzenai-hybrid">amd/Qwen1.5-7B-Chat-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>8.83</td></tr>
</table>

</details>

<details>
<summary>Qwen-2.5-7B-Instruct-Hybrid</summary>

```bash
lemonade-server pull Qwen-2.5-7B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen2.5-7B-Instruct-onnx-ryzenai-hybrid">amd/Qwen2.5-7B-Instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>8.65</td></tr>
</table>

</details>

<details>
<summary>Qwen-2.5-3B-Instruct-Hybrid</summary>

```bash
lemonade-server pull Qwen-2.5-3B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen2.5-3B-Instruct-onnx-ryzenai-hybrid">amd/Qwen2.5-3B-Instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>3.97</td></tr>
</table>

</details>

<details>
<summary>Qwen-2.5-1.5B-Instruct-Hybrid</summary>

```bash
lemonade-server pull Qwen-2.5-1.5B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen2.5-1.5B-Instruct-onnx-ryzenai-hybrid">amd/Qwen2.5-1.5B-Instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>2.16</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-R1-Distill-Llama-8B-Hybrid</summary>

```bash
lemonade-server pull DeepSeek-R1-Distill-Llama-8B-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-onnx-ryzenai-hybrid">amd/DeepSeek-R1-Distill-Llama-8B-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>9.09</td></tr>
</table>

</details>

<details>
<summary>Mistral-7B-v0.3-Instruct-Hybrid</summary>

```bash
lemonade-server pull Mistral-7B-v0.3-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Mistral-7B-Instruct-v0.3-onnx-ryzenai-hybrid">amd/Mistral-7B-Instruct-v0.3-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>7.85</td></tr>
</table>

</details>

<details>
<summary>Llama-3.1-8B-Instruct-Hybrid</summary>

```bash
lemonade-server pull Llama-3.1-8B-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Meta-Llama-3.1-8B-Instruct-onnx-ryzenai-hybrid">amd/Meta-Llama-3.1-8B-Instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>9.09</td></tr>
</table>

</details>

<details>
<summary>Qwen3-1.7B-Hybrid</summary>

```bash
lemonade-server pull Qwen3-1.7B-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen3-1.7B-awq-quant-onnx-hybrid">amd/Qwen3-1.7B-awq-quant-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>2.55</td></tr>
</table>

</details>

<details>
<summary>Phi-4-Mini-Instruct-Hybrid</summary>

```bash
lemonade-server pull Phi-4-Mini-Instruct-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Phi-4-mini-instruct-onnx-ryzenai-hybrid">amd/Phi-4-mini-instruct-onnx-ryzenai-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Size (GB)</td><td>5.46</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-Hybrid</summary>

```bash
lemonade-server pull Qwen3-4B-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen3-4B-awq-quant-onnx-hybrid">amd/Qwen3-4B-awq-quant-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>5.17</td></tr>
</table>

</details>

<details>
<summary>Qwen3-8B-Hybrid</summary>

```bash
lemonade-server pull Qwen3-8B-Hybrid
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen3-8B-awq-quant-onnx-hybrid">amd/Qwen3-8B-awq-quant-onnx-hybrid</a></td></tr>
<tr><td>Recipe</td><td>oga-hybrid</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>9.42</td></tr>
</table>

</details>


### Ryzen AI NPU

<details>
<summary>Qwen-2.5-7B-Instruct-NPU</summary>

```bash
lemonade-server pull Qwen-2.5-7B-Instruct-NPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen2.5-7B-Instruct-onnx-ryzenai-npu">amd/Qwen2.5-7B-Instruct-onnx-ryzenai-npu</a></td></tr>
<tr><td>Recipe</td><td>oga-npu</td></tr>
<tr><td>Size (GB)</td><td>8.82</td></tr>
</table>

</details>

<details>
<summary>Qwen-2.5-3B-Instruct-NPU</summary>

```bash
lemonade-server pull Qwen-2.5-3B-Instruct-NPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Qwen2.5-3B-Instruct-onnx-ryzenai-npu">amd/Qwen2.5-3B-Instruct-onnx-ryzenai-npu</a></td></tr>
<tr><td>Recipe</td><td>oga-npu</td></tr>
<tr><td>Size (GB)</td><td>4.09</td></tr>
</table>

</details>

<details>
<summary>DeepSeek-R1-Distill-Llama-8B-NPU</summary>

```bash
lemonade-server pull DeepSeek-R1-Distill-Llama-8B-NPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/DeepSeek-R1-Distill-Llama-8B-onnx-ryzenai-npu">amd/DeepSeek-R1-Distill-Llama-8B-onnx-ryzenai-npu</a></td></tr>
<tr><td>Recipe</td><td>oga-npu</td></tr>
<tr><td>Size (GB)</td><td>9.3</td></tr>
</table>

</details>

<details>
<summary>Mistral-7B-v0.3-Instruct-NPU</summary>

```bash
lemonade-server pull Mistral-7B-v0.3-Instruct-NPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Mistral-7B-Instruct-v0.3-onnx-ryzenai-npu">amd/Mistral-7B-Instruct-v0.3-onnx-ryzenai-npu</a></td></tr>
<tr><td>Recipe</td><td>oga-npu</td></tr>
<tr><td>Size (GB)</td><td>8.09</td></tr>
</table>

</details>

<details>
<summary>Phi-3.5-Mini-Instruct-NPU</summary>

```bash
lemonade-server pull Phi-3.5-Mini-Instruct-NPU
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td><a href="https://huggingface.co/amd/Phi-3.5-mini-instruct-onnx-ryzenai-npu">amd/Phi-3.5-mini-instruct-onnx-ryzenai-npu</a></td></tr>
<tr><td>Recipe</td><td>oga-npu</td></tr>
<tr><td>Size (GB)</td><td>4.35</td></tr>
</table>

</details>


### FastFlowLM (NPU)

<details>
<summary>gpt-oss-20b-FLM</summary>

```bash
lemonade-server pull gpt-oss-20b-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>gpt-oss:20b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>13.4</td></tr>
</table>

</details>

<details>
<summary>Gemma3-1b-it-FLM</summary>

```bash
lemonade-server pull Gemma3-1b-it-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>gemma3:1b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Size (GB)</td><td>1.17</td></tr>
</table>

</details>

<details>
<summary>Gemma3-4b-it-FLM</summary>

```bash
lemonade-server pull Gemma3-4b-it-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>gemma3:4b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>hot, vision</td></tr>
<tr><td>Size (GB)</td><td>5.26</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-VL-FLM</summary>

```bash
lemonade-server pull Qwen3-4B-VL-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>qwen3vl-it:4b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>hot, vision</td></tr>
<tr><td>Size (GB)</td><td>3.85</td></tr>
</table>

</details>

<details>
<summary>Qwen3-0.6b-FLM</summary>

```bash
lemonade-server pull Qwen3-0.6b-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>qwen3:0.6b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>0.66</td></tr>
</table>

</details>

<details>
<summary>Qwen3-4B-Instruct-2507-FLM</summary>

```bash
lemonade-server pull Qwen3-4B-Instruct-2507-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>qwen3-it:4b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Size (GB)</td><td>3.07</td></tr>
</table>

</details>

<details>
<summary>Qwen3-8b-FLM</summary>

```bash
lemonade-server pull Qwen3-8b-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>qwen3:8b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>5.57</td></tr>
</table>

</details>

<details>
<summary>Llama-3.1-8B-FLM</summary>

```bash
lemonade-server pull Llama-3.1-8B-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>llama3.1:8b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Size (GB)</td><td>5.36</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-1B-FLM</summary>

```bash
lemonade-server pull Llama-3.2-1B-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>llama3.2:1b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Size (GB)</td><td>1.21</td></tr>
</table>

</details>

<details>
<summary>Llama-3.2-3B-FLM</summary>

```bash
lemonade-server pull Llama-3.2-3B-FLM
```

<table>
<tr><th>Key</th><th>Value</th></tr>
<tr><td>Checkpoint</td><td>llama3.2:3b</td></tr>
<tr><td>Recipe</td><td>flm</td></tr>
<tr><td>Size (GB)</td><td>2.62</td></tr>
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
<tr><td>Size (GB)</td><td>0.77</td></tr>
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
<tr><td>Size (GB)</td><td>2.23</td></tr>
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
<tr><td>Size (GB)</td><td>5.89</td></tr>
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
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>5.78</td></tr>
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
<tr><td>Labels</td><td>reasoning</td></tr>
<tr><td>Size (GB)</td><td>5.78</td></tr>
</table>

</details>



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

> Note: `lemonade-server` is a utility that is added to your PATH when you install Lemonade Server.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->