# 🍋 Lemonade Frequently Asked Questions

## Overview

### 1. **What is Lemonade and what does it include?**

   Lemonade is an open-source local LLM solution that:
      - Gets you started in minutes with one-click installers.
      - Auto-configures optimized inference engines for your PC.
      - Provides a convenient app to get set up and test out LLMs.
      - Provides LLMs through the OpenAI API standard, enabling apps on your PC to access them.

### 2. **What are the use cases for different audiences?**

   - **LLM Enthusiasts**: LLMs on your GPU or NPU with minimal setup, and connect to great apps listed [here](https://lemonade-server.ai/docs/server/apps/).
   - **Developers**: Integrate LLMs into apps using standard APIs with no device-specific code. See the [Server Integration Guide](https://lemonade-server.ai/docs/server/server_integration/
   ).
   - **Agent Developers**: Use [GAIA](https://github.com/amd/gaia) to quickly develop local-first agents.

## Installation & Compatibility

### 1. **How do I install Lemonade SDK or Server?**

   Visit https://lemonade-server.ai/install_options.html and click the options that apply to you.

### 2. **Which devices are supported?**

   👉 [Supported Configurations](https://github.com/lemonade-sdk/lemonade?tab=readme-ov-file#supported-configurations)

   For more information on AMD Ryzen AI NPU Support, see the section [Hybrid/NPU](#hybrid-and-npu-questions).

### 3. **Is Linux supported? What about macOS?**

   Yes, Linux is supported!
   
   - **Linux**: Visit https://lemonade-server.ai/ and check the "Developer Setup" section for installation instructions.
   - **macOS**: Not supported right now, but it is on the roadmap.
   
   Visit the [Supported Configurations](https://github.com/lemonade-sdk/lemonade?tab=readme-ov-file#supported-configurations) section to see the support matrix for CPU, GPU, and NPU.

### 4. **How do I uninstall Lemonade Server? (Windows)**

   To uninstall Lemonade Server, use the Windows Add/Remove Programs menu. 

   **Optional: Remove cached files**
   - Open File Explorer and navigate to `%USERPROFILE%\.cache`
   - Delete the `lemonade` folder if it exists
   - To remove downloaded models, delete the `huggingface` folder

## Models

### 1. **Where are models stored and how do I change that?**

   Lemonade uses three model locations:

   **Primary: Hugging Face Cache**
   
   Models downloaded through Lemonade are stored using the Hugging Face Hub specification. By default, models are located at `~/.cache/huggingface/hub/`, where `~` is your home directory.

   For example, `Qwen/Qwen2.5-0.5B` is stored at `~/.cache/huggingface/hub/models--Qwen--Qwen2.5-0.5B`.

   You can change this location by setting the `HF_HOME` env var, which will store your models in `$HF_HOME/hub` (e.g., `$HF_HOME/hub/models--Qwen--Qwen2.5-0.5B`). Alternatively, you can set `HF_HUB_CACHE` and your models will be in `$HF_HUB_CACHE` (e.g., `$HF_HUB_CACHE/models--Qwen--Qwen2.5-0.5B`).

   You can use the official Hugging Face Hub utility (`pip install huggingface-hub`) to manage models outside of Lemonade, e.g., `hf cache ls` will print all models and their sizes.

   **Secondary: Extra Models Directory (GGUF)**

   Lemonade Server can discover GGUF models from a secondary directory using the `--extra-models-dir` option, enabling compatibility with llama.cpp and LM Studio model caches. Suggested paths:
   
   - **Windows:** 
       - LM Studio: `C:\Users\You\.lmstudio\models`
       - llamacpp: `%LOCALAPPDATA%\llama.cpp` (e.g., `C:\Users\You\AppData\Local\llama.cpp`)
   - **Linux:** `~/.cache/llama.cpp`

   Example: `lemonade-server serve --extra-models-dir "%LOCALAPPDATA%\llama.cpp"`

   Any `.gguf` files found in this directory (including subdirectories) will automatically appear in Lemonade's model list in the `custom` category.

   **FastFlowLM**

   FastFlowLM (FLM) has its own model management system. When you first install FLM the install wizard asks for a model directory, which is then saved to the `FLM_MODEL_PATH` environment variable on your system PATH. Models are stored in that directory. If you change the variable's value, newly downloaded models will be stored on the new path, but your prior models will still be at the prior path.  

### 2. **What models are supported?**

   Lemonade supports a wide range of LLMs including LLaMA, DeepSeek, Qwen, Gemma, Phi, gpt-oss, LFM, and many more. Most GGUF models can also be added to Lemonade Server by users using the Model Manager interface in the app or the `pull` command on the CLI.
   
   👉 [Supported Models List](https://lemonade-server.ai/docs/server/server_models/)
   👉 [pull command](https://lemonade-server.ai/docs/server/lemonade-server-cli/#pull-command-options)

### 3. **How do I know what size model will work with my setup?**

   Model compatibility depends on your system's RAM, VRAM, and NPU availability. **The actual file size varies significantly between models** due to different quantization techniques and architectures.

   **To check if a model will work:**
   1. Visit the model's Hugging Face page (e.g., [`amd/Qwen2.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid`](https://huggingface.co/amd/Qwen2.5-7B-Chat-awq-g128-int4-asym-fp16-onnx-hybrid)).
   2. Check the "Files and versions" tab to see the actual download size.
   3. Add ~2-4 GB overhead for KV cache, activations, and runtime memory.
   4. Ensure your system has sufficient RAM/VRAM.

### 4. **I'm looking for a model, but it's not listed in the Model Manager.**

   If a model isn't listed, it may not be compatible with your PC due to device or RAM limitations, or we just haven't added it to the `server_models.json` file yet.
   
   You can:

   - Add a custom model manually via the app's "Add a Model" interface or the [CLI pull command](https://lemonade-server.ai/docs/server/lemonade-server-cli/#pull-command-options).
   - Use a pull request to add the model to the built-in `server_models.json` file.
   - Request support by opening a [GitHub issue](https://github.com/lemonade-sdk/lemonade/issues).

   If you are sure that a model should be listed, but you aren't seeing it, you can set the `LEMONADE_DISABLE_MODEL_FILTERING` environment variable to show all models supported by Lemonade on any PC configuration. But please note, this can show models that definitely won't work on your system.

### 5. **Is there a script or tool to convert models to Ryzen AI NPU format?**

   Yes, there's a guide on preparing your models for Ryzen AI NPU:

   👉 [Model Preparation Guide](https://ryzenai.docs.amd.com/en/latest/oga_model_prepare.html)

### 6. **What's the difference between GGUF and ONNX models?**

   - **GGUF**: Used with llama.cpp backend, supports CPU, and GPU via Vulkan or ROCm.
   - **ONNX**: Used with OnnxRuntime GenAI, supports NPU and NPU+iGPU Hybrid execution.

## Inference Behavior & Performance

### 1. **Can Lemonade print out stats like tokens per second?**

   Yes! Lemonade Server exposes a `/stats` endpoint that returns performance metrics from the most recent completion request:

   ```bash
   curl http://localhost:8000/api/v1/stats
   ```

   Or, you can launch `lemonade-server` with the option `--log-level debug` and that will also print out stats.

### 2. **How does Lemonade's performance compare to llama.cpp?**

   Lemonade supports llama.cpp as a backend, so performance is similar when using the same model and quantization.

### 3. **How can ROCm performance be improved for my use case?**

   File a detailed issue on TheRock repo for support: https://github.com/ROCm/TheRock

## Hybrid and NPU Questions

### 1. **Does LLM inference with the NPU only work on Windows?**

   Yes, today, NPU and hybrid inference is currently supported only on Windows.

   To request NPU support on Linux, file an issue with either:
     - Ryzen AI SW: https://github.com/amd/ryzenai-sw
     - FastFlowLM: https://github.com/FastFlowLM/FastFlowLM

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
   - **Intel/NVIDIA systems**: CPU inference, with GPU support via the llama.cpp + Vulkan backend

   While you won't get NPU acceleration on non-Ryzen AI 300 systems, you can still benefit from GPU acceleration and the OpenAI-compatible API.
   
### 4. **Is the NPU on the AMD Ryzen AI 7000/8000/200 series going to be supported for LLM inference?**

   No inference engine providers have plans to support NPUs prior to Ryzen AI 300-series, but you can still request this by filing an issue on their respective GitHubs:
      - Ryzen AI SW: https://github.com/amd/ryzenai-sw
      - FastFlowLM: https://github.com/FastFlowLM/FastFlowLM

### 5. **How do I know what model architectures are supported by the NPU?**

   AMD publishes pre-quantized and optimized models in their Hugging Face collections:

   - [Ryzen AI NPU Models](https://huggingface.co/collections/amd/ryzenai-15-llm-npu-models-6859846d7c13f81298990db0)
   - [Ryzen AI Hybrid Models](https://huggingface.co/collections/amd/ryzenai-15-llm-hybrid-models-6859a64b421b5c27e1e53899)

   To find the architecture of a specific model, click on any model in these collections and look for the "Base model" field, which will show you the underlying architecture (e.g., Llama, Qwen, Phi).

### 6. **How can I get better performance from the NPU?**

   Make sure that you've put the NPU in "Turbo" mode to get the best results. This is done by opening a terminal window and running the following commands:

   ```cmd
   cd C:\Windows\System32\AMD
   .\xrt-smi configure --pmode turbo
   ```

## Support & Roadmap

### 1. **What if I encounter installation or runtime errors?**

   Check the Lemonade Server logs via the App (all supported OSes) or tray icon (Windows only). Common issues include model compatibility or outdated versions.
   
   👉 [Open an Issue on GitHub](https://github.com/lemonade-sdk/lemonade/issues)

### 2. **Lemonade is missing a feature I really want. What should I do?**

   Open a feature request on GitHub. We're actively shaping the roadmap based on user feedback.

### 3. **Do you plan to share a roadmap?**

   Yes! Check out the project README:
   
   👉 [Lemonade Roadmap](https://github.com/lemonade-sdk/lemonade#project-roadmap)
