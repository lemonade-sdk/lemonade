# Introduction

onnxruntime-genai (aka OGA) is a new framework created by Microsoft for running ONNX LLMs: https://github.com/microsoft/onnxruntime-genai/tree/main?tab=readme-ov-file

## NPU instructions

### Warnings

 - The OGA wheels need to be installed in a specific order or you will end up with the wrong packages in your environment. If you see pip dependency errors, please delete your conda env and start over with a fresh environment.

### Installation

1. NOTE: ⚠️ DO THESE STEPS IN EXACTLY THIS ORDER ⚠️
1. Install `lemonade`:
    1. Create a conda environment: `conda create -n oga-npu python=3.10` (Python 3.10 is required)
    1. Activate: `conda activate oga-npu`
    1. `cd REPO_ROOT`
    1. `pip install -e .[oga-npu]`
1. Download required OGA packages
    1. Access the [AMD RyzenAI EA Lounge](https://account.amd.com/en/member/ryzenai-sw-ea.html#tabs-a5e122f973-item-4757898120-tab) and  download `amd_oga_Oct4_2024.zip` from `Ryzen AI 1.3 EA Release`.
    1. Unzip `amd_oga_Oct4_2024.zip`
1. Setup your folder structure:
    1. Copy the `amd_oga` folder from the above zip file, if desired
    1. Create the system environment variable `AMD_OGA` and set it to the path to the `amd_oga` folder
1. Install the wheels:
    1. `cd %AMD_OGA%\wheels`
    1. `pip install onnxruntime_genai-0.5.0.dev0-cp310-cp310-win_amd64.whl`
    1. `pip install onnxruntime_vitisai-1.20.0-cp310-cp310-win_amd64.whl`
    1. `pip install voe-1.2.0-cp310-cp310-win_amd64.whl`
1. Install driver
    1. Access the [AMD RyzenAI EA Lounge](https://account.amd.com/en/member/ryzenai-sw-ea.html#tabs-a5e122f973-item-4757898120-tab) and  download `Win24AIDriver.zip` from `Ryzen AI 1.3 Preview Release`.
    1. Unzip `Win24AIDriver.zip`
    1. Right click `kipudrv.inf` and select `Install`
    1. Check under `Device Manager` to ensure that `NPU Compute Accelerator` is using version `32.0.203.219`.

### Runtime

To test basic functionality, point lemonade to any of the models under [quark-quantized-onnx-llms-for-ryzen-ai-1.3-ea](https://huggingface.co/collections/amd/quark-quantized-onnx-llms-for-ryzen-ai-13-ea-66fc8e24927ec45504381902):

```
lemonade -i amd/Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix oga-load --device npu --dtype int4 llm-prompt -p "hello whats your name?" --max-new-tokens 15
```

```
Building "amd_Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix"
[Vitis AI EP] No. of Operators :   CPU    73 MATMULNBITS    99
[Vitis AI EP] No. of Subgraphs :MATMULNBITS    33
    ✓ Loading OnnxRuntime-GenAI model
    ✓ Prompting LLM

amd/Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix:
        <built-in function input> (executed 1x)
                Build dir:      C:\Users\danie/.cache/lemonade\amd_Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix
                Status:         Successful build!
                                Dtype:  int4
                                Device: npu
                                Response:       hello whats your name?
Hi, I'm a 21 year old male from the
```

To test/use the websocket server:

```
lemonade -i amd/Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix oga-load --device npu --dtype int4 serve --max-new-tokens 50
```

Then open the address (http://localhost:8000) in a browser and chat with it.

```
Building "amd_Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix"
[Vitis AI EP] No. of Operators :   CPU    73 MATMULNBITS    99
[Vitis AI EP] No. of Subgraphs :MATMULNBITS    33
    ✓ Loading OnnxRuntime-GenAI model


INFO:     Started server process [27752]
INFO:     Waiting for application startup.
INFO:     Application startup complete.
INFO:     Uvicorn running on http://localhost:8000 (Press CTRL+C to quit)
INFO:     ::1:54973 - "GET / HTTP/1.1" 200 OK
INFO:     ('::1', 54975) - "WebSocket /ws" [accepted]
INFO:     connection open
I'm a newbie here. I'm looking for a good place to buy a domain name. I've been looking around and i've found a few good places.
```

To run a single MMLU test:

```
lemonade -i amd/Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix oga-load --device npu --dtype int4 accuracy-mmlu --tests management
```

```
Building "amd_Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix"
[Vitis AI EP] No. of Operators :   CPU    73 MATMULNBITS    99
[Vitis AI EP] No. of Subgraphs :MATMULNBITS    33
    ✓ Loading OnnxRuntime-GenAI model   
    ✓ Measuring accuracy with MMLU   

amd/Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix:
        <built-in function input> (executed 1x)
                Build dir:      C:\Users\danie/.cache/lemonade\amd_Llama-2-7b-hf-awq-g128-int4-asym-fp32-onnx-ryzen-strix    
                Status:         Successful build!
                                Dtype:  int4 
                                Device: npu 
                                Mmlu Management Accuracy:       56.31 %
```
