"""
This example demonstrates how to use the LEAP API to load a model for
inference on CPU using the hf-cpu recipe, and then use it to generate
the response to a prompt.

If you have a discrete GPU, you can try that by changing the recipe
to hf-dgpu. Note: make sure to have torch+cuda installed when trying
hf-dgpu.
"""

from lemonade import leap

model, tokenizer = leap.from_pretrained("facebook/opt-125m", recipe="hf-cpu")

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))
