"""
This example demonstrates how to use the lemonade API to load a model for
inference on Ryzen AI NPU via OnnxRuntime-Genai (OGA) using the oga-npu recipe,
and then use it to generate the response to a prompt.

Make sure you have set up your OGA device in your Python environment.
See for details:
https://github.com/lemonade-sdk/lemonade/blob/main/docs/README.md#installation
"""

from lemonade.api import from_pretrained

model, tokenizer = from_pretrained(
    "amd/Phi-3.5-mini-instruct-awq-g128-int4-asym-bf16-onnx-ryzen-strix",
    recipe="oga-npu",
)

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
