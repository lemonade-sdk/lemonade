# Installing the Lemonade SDK From Source Code

The following provides the steps to install Lemonade from source code. We also provide 2 alternative ways to install Lemonade:

* To install Lemonade via PyPi, see the [Lemonade README](README.md).
* To install Lemonade Server using the standalone GUI installer, see the [Lemonade Server README](server/README.md).

1. Clone: `git clone https://github.com/lemonade-sdk/lemonade.git`
1. `cd lemonade` (where `lemonade` is the repo root of your clone)
    - Note: be sure to run these installation instructions from the repo root.
1. Create and activate a [Miniconda](https://repo.anaconda.com/miniconda/Miniconda3-latest-Windows-x86_64.exe) environment.
    ```bash
    conda create -n lemon python=3.10
    ```

    ```bash
    conda activate lemon
    ```

3. Install Lemonade for your backend of choice: 
    - [OnnxRuntime GenAI with CPU backend](ort_genai_igpu.md): 
        ```bash
        pip install -e .[llm-oga-cpu]
        ```
    - [OnnxRuntime GenAI with Integrated GPU (iGPU, DirectML) backend](ort_genai_igpu.md):
        > Note: Requires Windows and a DirectML-compatible iGPU.
        ```bash
        pip install -e .[llm-oga-igpu]
        ```
    - OnnxRuntime GenAI with Ryzen AI Hybrid (NPU + iGPU) backend:
        > Note: Ryzen AI Hybrid requires a Windows 11 PC with an AMD Ryzen™ AI 300-series processor.
        > - Ensure you have the correct driver version installed by checking [here](https://ryzenai.docs.amd.com/en/latest/inst.html#install-npu-drivers).
        > - Visit the [AMD Hugging Face OGA Hybrid collection](https://huggingface.co/collections/amd/ryzenai-14-llm-hybrid-models-67da31231bba0f733750a99c) for supported checkpoints.
        ```bash
        pip install -e .[llm-oga-hybrid]
        ```

        ```bash
        lemonade-install --ryzenai hybrid
        ```
    - Hugging Face (PyTorch) LLMs for CPU backend:
        ```bash
            pip install -e .[llm]
        ```
    - llama.cpp: see [instructions](llamacpp.md).

4. Use `lemonade -h` to explore the LLM tools, and see the [commands](README.md#cli-commands) and [APIs](README.md#api) in the [Lemonade SDK REAMDE](README.md).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->