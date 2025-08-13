# 🍋 Lemonade Frequently Asked Questions

## Overview

### 1. **What is Lemonade SDK and what does it include?**

   Lemonade is an open-source SDK that provides high-level APIs, CLI tools, and a server interface to deploy and benchmark LLMs using ONNX Runtime GenAI (OGA), Hugging Face Transformers, and llama.cpp backends.

### 2. **What is Lemonade Server and how is it different from the SDK?**

   Lemonade Server is a component of the SDK that enables local LLM deployment via an OpenAI-compatible API. It allows integration with apps like chatbots and coding assistants without requiring code changes. It's available as a standalone Windows GUI installer or via command line for Linux.

### 3. **What are the use cases for different audiences?**

   - **End Users**: Use [GAIA](https://github.com/amd/gaia) for a Chatbot experience locally.
   - **LLM Enthusiasts**: LLMs on your GPU or NPU with minimal setup, and connect to great apps listed [here](https://lemonade-server.ai/docs/server/apps/).
   - **Developers**: Integrate LLMs into apps using standard APIs with no device-specific code. See the [Server Integration Guide](https://lemonade-server.ai/docs/server/server_integration/
   ).

## Installation & Compatibility

### 1. **How do I install Lemonade SDK or Server?**

   Visit https://lemonade-server.ai/install_options.html and click the options that apply to you.

### 2. **Which devices are supported?**

   👉 [Supported Configurations](https://github.com/lemonade-sdk/lemonade?tab=readme-ov-file#supported-configurations)

   For more information on Hybrid/NPU Support, see the section [Hybrid/NPU](#hybrid-and-npu-questions).

### 3. **Is Linux supported?**

   Yes! To install Lemonade on Linux, visit https://lemonade-server.ai/ and check the "Developer Setup" section for installation instructions. Visit the [Supported Configurations](https://github.com/lemonade-sdk/lemonade?tab=readme-ov-file#supported-configurations) section to see the support matrix for CPU, GPU, and NPU.

### 4. **How do I uninstall Lemonade Server? (Windows)**

   To completely uninstall Lemonade Server from your system, follow these steps:

   **Step 1: Remove cached files**
   - Open File Explorer and navigate to `%USERPROFILE%\.cache`
   - Delete the `lemonade` folder if it exists
   - [Optional] To remove downloaded models, delete the `huggingface` folder

   **Step 2: Remove from PATH environment variable**
   - Press `Win + I` to open Windows Settings
   - Search for "environment variables" and select "Edit environment variables for your account"
   - Find "Path" in the list and click "Edit"
   - Look for the entry containing `lemonade_server\bin` and select it
   - Click "Delete" then "OK"

   **Step 3: Delete installation folder**
   - Navigate to `%LOCALAPPDATA%\lemonade_server`
   - Delete the entire `lemonade_server` folder

## Models & Performance

### 1. **What models are supported?**

   Lemonade supports a wide range of LLMs including LLaMA, DeepSeek, Qwen, Gemma, Phi, and gpt-oss. Most GGUF models can also be added to Lemonade Server by users using the Model Manager interface.
   
   👉 [Supported Models List](https://lemonade-server.ai/docs/server/server_models/)

### 2. **How do I know what size model will work with my setup?**

   Model compatibility depends on your system's RAM, VRAM, and NPU availability. **The actual file size varies significantly between models** due to different quantization techniques and architectures.

   **To check if a model will work:**
   1. Visit the model's Hugging Face page (e.g., [`amd/Qwen2.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid`](https://huggingface.co/amd/Qwen2.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid)).
   2. Check the "Files and versions" tab to see the actual download size.
   3. Add ~2-4 GB overhead for KV cache, activations, and runtime memory.
   4. Ensure your system has sufficient RAM/VRAM.

### 3. **I'm looking for a model, but it's not listed in the Model Manager.**

   If a model isn't listed, it may not yet be validated or compatible with your selected backend (for example, Hybrid models will not show if Ryzen AI Hybrid software is not installed). You can:

   - Add a custom model manually via the Lemonade Server Model Manager's "Add a Model" interface.
   - Request support by opening a [GitHub issue](https://github.com/lemonade-sdk/lemonade/issues).

### 4. **Is there a script or tool to convert models to hybrid format?**

   Yes, there's a guide on preparing your models for Ryzen AI NPU:

   👉 [Model Preparation Guide](https://ryzenai.docs.amd.com/en/latest/oga_model_prepare.html)

### 5. **What's the difference between GGUF and ONNX models?**

   - **GGUF**: Used with llama.cpp backend, supports CPU, and GPU via Vulkan or ROCm.
   - **ONNX**: Used with OnnxRuntime GenAI, supports NPU and NPU+iGPU Hybrid execution.

## Inference Behavior & Performance

### 1. **What are the performance benchmarks that can be collected using Lemonade SDK?**

   You can measure:
   
   - Inference speed
   - Time to first token
   - Tokens per second
   - Accuracy via MMLU and other benchmarks

### 2. **Can Lemonade print out stats like tokens per second?**

   Yes! Lemonade Server exposes a `/stats` endpoint that returns performance metrics from the most recent completion request:

   ```bash
   curl http://localhost:8000/api/v1/stats
   ```

   Or, you can launch `lemonade-server` with the option `--log-level debug` and that will also print out stats.

### 3. **How does Lemonade's performance compare to llama.cpp?**

   Lemonade supports llama.cpp as a backend, so performance is similar when using the same model and quantization.

## Hybrid and NPU Questions

### 1. **Does hybrid inference with the NPU only work on Windows?**

   Yes, hybrid inference is currently supported only on Windows. NPU-only inference is coming to Linux soon, followed by hybrid (NPU+iGPU) support via ROCm.

### 2. **I loaded a hybrid model, but the NPU is barely active. Is that expected?**

   Yes. In hybrid mode:
   
   - The NPU handles prompt processing.
   - The GPU handles token generation.
   - If your prompt is short, the NPU finishes quickly. Try a longer prompt to see more NPU activity.

### 3. **Does Lemonade work on older AMD processors or non-Ryzen AI systems?**

   Yes! Lemonade supports multiple execution modes:
   
   - **AMD Ryzen 7000/8000/200 series**: GPU acceleration via llama.cpp + Vulkan backend
   - **Systems with Radeon GPUs**: Yes
   - **Any x86 CPU**: Yes
   - **Intel/NVIDIA systems**: CPU inference, with GPU support if compatible drivers are available
   
   While you won't get NPU acceleration on non-Ryzen AI 300 systems, you can still benefit from GPU acceleration and the OpenAI-compatible API.

### 4. **How do I know what model architectures are supported by the NPU?**

   AMD publishes pre-quantized and optimized models in their Hugging Face collections:

   - [Ryzen AI NPU Models](https://huggingface.co/collections/amd/ryzenai-15-llm-npu-models-6859846d7c13f81298990db0)
   - [Ryzen AI Hybrid Models](https://huggingface.co/collections/amd/ryzenai-15-llm-hybrid-models-6859a64b421b5c27e1e53899)

   To find the architecture of a specific model, click on any model in these collections and look for the "Base model" field, which will show you the underlying architecture (e.g., Llama, Qwen, Phi).

### 5. **How can I get better performance from the NPU?**

   Make sure that you've put the NPU in "Turbo" mode to get the best results. This is done by opening a terminal window and running the following commands:

   ```cmd
   cd C:\Windows\System32\AMD
   .\xrt-smi configure --pmode turbo
   ```

## Support & Roadmap

### 1. **What if I encounter installation or runtime errors?**

   Check the Lemonade Server logs via the tray icon. Common issues include model compatibility or outdated versions.
   
   👉 [Open an Issue on GitHub](https://github.com/lemonade-sdk/lemonade/issues)

### 2. **Lemonade is missing a feature I really want. What should I do?**

   Open a feature request on GitHub. We're actively shaping the roadmap based on user feedback.

### 3. **Do you plan to share a roadmap?**

   Yes! We tag roadmap items on GitHub with the "on roadmap" label.
   
   👉 [Lemonade SDK Roadmap Issues](https://github.com/lemonade-sdk/lemonade/issues?q=is%3Aissue%20state%3Aopen%20label%3A"on%20roadmap")
