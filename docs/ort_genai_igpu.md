# OnnxRuntime GenAI (OGA) for iGPU and CPU

[onnxruntime-genai (aka OGA)](https://github.com/microsoft/onnxruntime-genai/tree/main?tab=readme-ov-file) is a new framework created by Microsoft for running ONNX LLMs.

## Installation

See [Lemonade Installation](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/README.md#install) for the OGA iGPU backend.

## Get models

- The oga-load tool can download models from Hugging Face and build ONNX files using OGA's `model_builder`, which can quantize and optimize models for both iGPU and CPU.
- Download and build ONNX model files:
  - `lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4`
  - `lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device cpu --dtype int4`
- The ONNX model files will be stored in the respective subfolder of the lemonade cache folder and will be reused in future oga-load calls:
  - `oga_models\microsoft_phi-3-mini-4k-instruct\dml-int4`
  - `oga_models\microsoft_phi-3-mini-4k-instruct\cpu-int4`
- The ONNX model build process can be forced to run again, overwriting the above cache, by using the `--force` flag:
  - `lemonade -i microsoft/Phi-3-mini-4k-instruct oga-load --device igpu --dtype int4 --force`
- Transformer model architectures supported by the model_builder tool include many popular state-of-the-art models, such as:
  - Gemma
  - LLaMa
  - Mistral
  - Phi
  - Qwen
  - Nemotron
- For the full list of supported models, please see the [model_builder documentation](https://github.com/microsoft/onnxruntime-genai/blob/main/src/python/py/models/README.md).
- The following quantizations are supported for automatically building ONNXRuntime GenAI model files from the Hugging Face repository:
  - `cpu`: `fp32`, `int4`
  - `igpu`: `fp16`, `int4`

## Directory structure:
- The model_builder tool caches Hugging Face files and temporary ONNX external data files in `<LEMONADE CACHE>\model_builder`
- The output from model_builder is stored in `<LEMONADE_CACHE>\oga_models\<MODELNAME>\<SUBFOLDER>`
  - `MODELNAME` is the Hugging Face checkpoint name where any '/' is mapped to an '_' and everything is lower case.
  - `SUBFOLDER` is `<EP>-<DTYPE>`, where `EP` is the execution provider (`dml` for `igpu`, `cpu` for `cpu`, and `npu` for `npu`) and `DTYPE` is the datatype.
  - If the `--int4-block-size` flag is used then `SUBFOLDER` is` <EP>-<DTYPE>-block-<SIZE>` where `SIZE` is the specified block size.
- Other ONNX models in the format required by onnxruntime-genai can be loaded by Lemonade if placed in the `<LEMONADE_CACHE>\oga_models` folder.
  - Use the `-i` and `--subfolder` flags to specify the folder and subfolder, for example:
    - `lemonade -i my_model_name --subfolder my_subfolder --device igpu --dtype int4 oga-load`
  - Lemonade will expect the ONNX model files to be located in `<LEMONADE_CACHE>\oga_models\my_model_name\my_subfolder`
