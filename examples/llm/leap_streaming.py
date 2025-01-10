"""
This example demonstrates how to use the LEAP API to load a model for
inference on CPU using the hf-cpu recipe, and then use a thread to
generate a streaming the response to a prompt.

Note: this approach only works with recipes that support TextIteratorStreamer,
i.e., huggingface-based recipes such as hf-cpu and ryzenai-npu.
"""

from thread import Thread
from transformers import TextIteratorStreamer
from lemonade import leap

# Replace the recipe with "ryzenai-npu" to run on the RyzenAI NPU
model, tokenizer = leap.from_pretrained(
    "meta-llama/Llama-2-7b-chat-hf", recipe="hf-cpu"
)

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids

streamer = TextIteratorStreamer(
    tokenizer,
    skip_prompt=True,
)
generation_kwargs = {
    "input_ids": input_ids,
    "streamer": streamer,
    "max_new_tokens": 30,
}

thread = Thread(target=model.generate, kwargs=generation_kwargs)
thread.start()

# Generate the response using streaming
for new_text in streamer:
    print(new_text)

thread.join()
