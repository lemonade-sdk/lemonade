"""
This example demonstrates how to use the LEAP API to load a model for
inference on a Ryzen AI NPU using the ryzenai-npu-load recipe, 
and then use it to generate the response to a prompt.

Note that this example will only run if the Ryzen AI NPU Private recipe is installed.
See genai/docs/ryzenai_npu.md for instructions.

You can try the same model on CPU by changing the recipe to "hf-cpu".
"""

from lemonade import leap

model, tokenizer = leap.from_pretrained(
    "meta-llama/Llama-2-7b-chat-hf", recipe="ryzenai-npu"
)

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids
response = model.generate(input_ids, max_new_tokens=30)

print(tokenizer.decode(response[0]))
