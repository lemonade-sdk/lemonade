# LLAMA.CPP

Run transformer models using llama.cpp. This integration allows you to:
1. Load and run llama.cpp models
2. Benchmark model performance
3. Use the models with other tools like chat or MMLU accuracy testing

Note: The tool will download and install the necessary platform llama.cpp
executable files the first time the `llamacpp-load` tool is used. 

## Installation

By default, if no backend is pre-installed, the `llamacpp-load` tool will automatically download and install the appropriate backend when first used.

You can pre-install llama.cpp binaries with a specific backend using the `lemonade-install` command:

```bash
# Install llama.cpp with ROCm backend
lemonade-install --llamacpp rocm

# Install llama.cpp with Vulkan backend (for broader GPU compatibility)
lemonade-install --llamacpp vulkan
```

### Supported GPU Architectures

The ROCm backend supports various AMD GPU architectures including:
- **gfx1151 (STX Halo APU)** - Ryzen AI MAX 385-395
- **gfx1150 (STX Point APU)** - Ryzen AI 360-375
- **gfx120X (RDNA4 GPUs)** - includes AMD Radeon RX 9070 XT/GRE/9070, RX 9060 XT/9060
- **gfx110X (RDNA3 GPUs)** - includes AMD Radeon PRO W7900/W7800/W7700/W7600, RX 7900 XTX/XT/GRE, RX 7800 XT, RX 7700 XT/7700, RX 7600 XT/7600

## Get Models

The `llamacpp-load` tool can download GGUF model checkpoints from Hugging Face.  Use
the checkpoint name followed by the desired variant:

```bash
lemonade -i unsloth/Qwen3-0.6B-GGUF:Q4_0 llamacpp-load
```
The model is cached locally in the HuggingFace hub.

By default the tool will load the model ready for inference on the iGPU.
You can specify inference on the CPU by using the `--device` flag:

```bash
lemonade -i unsloth/Qwen3-0.6B-GGUF:Q4_0 llamacpp-load --device cpu
```

The `llamacpp-load` tool can also load a GGUF file.  You can specify the filename or folder,
if there is only one GGUF file in the folder:
```
lemonade -i models/my_gguf_model/my_model-Q4_0.gguf llamacpp-load

lemonade -i models/my_gguf_model llamacpp-load
```
Please see `lemonade llamacpp-load -h` for more options.

## Usage


### Benchmarking

After loading a model, you can benchmark it using `llamacpp-bench`:

```
lemonade -i <MODEL_CHECKPOINT:VARIANT> llamacpp-load llamacpp-bench
```
The benchmark will measure and report:
- Time to first token (prompt evaluation time)
- Token generation speed (tokens per second)
- Maximum memory used for each prompt by the external `llama-bench.exe` or `llama-cli.exe` application.  Note that the `Peak memory` reported in the status output is only for the `lemonade` execution.

By default, the `llamacpp-bench` tool will use the `llama-bench.exe` executable for benchmarking.  Use the 
`--cli` flag to use `llama-cli.exe`.  In this case, the bench tool will make multiple calls to the executable,
one per sample, which will have lower overall performance.


### Integration with Other Tools

After loading with `llamacpp-load`, the model can be used with any tool that supports the ModelAdapter interface, including:
- accuracy-mmlu
- llm-prompt
- accuracy-humaneval
- and more

The integration provides:
- Platform-independent path handling (works on Windows, Linux, and macOS)
- Proper error handling with detailed messages
- Performance metrics collection
- Configurable generation parameters (temperature, top_p, top_k)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->