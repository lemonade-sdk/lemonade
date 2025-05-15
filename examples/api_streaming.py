"""
This example demonstrates how to use the lemonade API to load a model for
inference on CPU using the hf-cpu recipe, and then use a thread to
generate a streaming the response to a prompt.

Note: this approach only works with recipes that support TextIteratorStreamer,
i.e., huggingface-based recipes such as hf-cpu and hf-dgpu.
"""

from threading import Thread
from transformers import TextIteratorStreamer
from lemonade.api import from_pretrained

model, tokenizer = from_pretrained("Qwen/Qwen2.5-0.5B-Instruct", recipe="hf-cpu")

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
