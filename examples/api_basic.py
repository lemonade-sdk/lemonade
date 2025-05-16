"""
This example demonstrates how to use the lemonade API to load a model for
inference on CPU using the hf-cpu recipe, and then use it to generate
the response to a prompt.

If you have a discrete GPU, you can try that by changing the recipe
to hf-dgpu. Note: make sure to have torch+cuda installed when trying
hf-dgpu.
"""

from lemonade.api import from_pretrained

model, tokenizer = from_pretrained("Qwen/Qwen2.5-0.5B-Instruct", recipe="hf-cpu")

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD
