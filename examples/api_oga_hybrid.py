"""
This example demonstrates how to use the lemonade API to load a model for
inference on Ryzen AI hybrid mode (NPU and iGPU together) via OnnxRuntime-Genai (OGA)
using the oga-hybrid recipe, and then use it to generate the response to a prompt.

Make sure you have set up your OGA device in your Python environment.
See for details:
https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/README.md#install
"""

from lemonade.api import from_pretrained

model, tokenizer = from_pretrained(
    "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid", recipe="oga-hybrid"
)

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))
