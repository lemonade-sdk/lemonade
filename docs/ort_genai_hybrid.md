# Introduction

[onnxruntime-genai (aka OGA)](https://github.com/microsoft/onnxruntime-genai/tree/main?tab=readme-ov-file) is a new framework created by Microsoft for running ONNX LLMs.

## Hybrid instructions

### Warnings

 - The OGA wheels need to be installed in a specific order or you will end up with the wrong packages in your environment. If you see pip dependency errors, please delete your conda env and start over with a fresh environment.

### Requirements
 - [NPU Drivers (version .237)](https://ryzenai.docs.amd.com/en/latest/inst.html#install-npu-drivers)
 - [Hybrid LLM artifacts package](https://account.amd.com/en/member/ryzenai-sw-ea.html#tabs-a5e122f973-item-4757898120-tab)

### Installation

1. NOTE: ⚠️ DO THESE STEPS IN EXACTLY THIS ORDER ⚠️
1. Install `lemonade`:
    1. Create a conda environment: `conda create -n oga-hybrid python=3.10` (Python 3.10 is required)
    1. Activate: `conda activate oga-hybrid`
    1. `cd REPO_ROOT`
    1. `pip install -e .[llm-oga-hybrid]`
1. Download required OGA packages
    1. Access the [Hybrid LLM artifacts package](https://account.amd.com/en/member/ryzenai-sw-ea.html#tabs-a5e122f973-item-4757898120-tab) and  download `hybrid-llm-artifacts_1.3.0.zip` and `onnxruntime_vitisai-1.19.0.dev20241217-cp310-cp310-win_amd64.whl`.
    1. Copy the `onnxruntime_vitisai-1.19.0.dev20241217-cp310-cp310-win_amd64.whl` file to the `hybrid-llm-artifacts_1.3.0\hybrid-llm-artifacts\onnxruntime_genai\wheel` folder.
    1. Unzip `hybrid-llm-artifacts_1.3.0.zip`
    1. Create the system environment variable `AMD_OGA_HYBRID` and set it to the path of the `hybrid-llm-artifacts_1.3.0` folder.
    1. Restart your terminal
1. Install the wheels:
    1. `cd hybrid-llm-artifacts_1.3.0\hybrid-llm-artifacts\onnxruntime_genai\wheel`
    1. `pip install onnxruntime_genai_directml-0.4.0.dev0-cp310-cp310-win_amd64.whl`
    1. `pip install onnxruntime_vitisai-1.19.0.dev20241217-cp310-cp310-win_amd64.whl`
1. Install driver
    1. Download NPU driver from [NPU Drivers (version .237)](https://ryzenai.docs.amd.com/en/latest/inst.html#install-npu-drivers)
    1. Unzip `NPU_RAI1.3.zip`
    1. Right click `kipudrv.inf` and select `Install`
    1. Check under `Device Manager` to ensure that `NPU Compute Accelerator` is using version `32.0.203.237`.

### Runtime

To test basic functionality, point lemonade to any of the models under [quark-quantized-onnx-hybrid-llms-for-ryzen-ai-1.3](https://huggingface.co/collections/amd/quark-awq-g128-int4-asym-fp16-onnx-hybrid-13-674b307d2ffa21dd68fa41d5):

```
lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid oga-load --device hybrid --dtype int4 llm-prompt -p "hello whats your name?" --max-new-tokens 15
```

```
Building "amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid"
    ✓ Loading OnnxRuntime-GenAI model
    ✓ Prompting LLM

amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid:
        <built-in function input> (executed 1x)
                Build dir:      C:\Users\ramkr\.cache\lemonade\amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid
                Status:         Successful build!
                                Dtype:  int4
                                Device: hybrid
                                Response:       hello whats your name? i'm a robot, and i'm here to help you with any questions



Woohoo! Saved to ~\.cache\lemonade\amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid
```

To test/use the websocket server:

```
lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid oga-load --device hybrid --dtype int4 serve --max-new-tokens 50
```

Then open the address (http://localhost:8000) in a browser and chat with it.

```
Building "amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid"
    ✓ Loading OnnxRuntime-GenAI model
      Launching LLM Server

INFO:     Started server process [8704]
INFO:     Waiting for application startup.
INFO:     Application startup complete.
INFO:     Uvicorn running on http://localhost:8000 (Press CTRL+C to quit)
INFO:     ::1:57038 - "GET / HTTP/1.1" 200 OK
INFO:     ('::1', 57042) - "WebSocket /ws" [accepted]
INFO:     connection open
```

To run a single MMLU test:

```
lemonade -i amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid oga-load --device hybrid --dtype int4 accuracy-mmlu --tests management
```

```
Building "amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid"
    ✓ Loading OnnxRuntime-GenAI model
    ✓ Measuring accuracy with MMLU

amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid:
        <built-in function input> (executed 1x)
                Build dir:      C:\Users\ramkr\.cache\lemonade\amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid
                Status:         Successful build!
                                Dtype:  int4
                                Device: hybrid
                                Mmlu Management Accuracy:       49.515 %



Woohoo! Saved to ~\.cache\lemonade\amd_Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid
```
