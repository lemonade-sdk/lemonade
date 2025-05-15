"""
This example demonstrates how to use the lemonade API to load a model for
inference on Ryzen AI hybrid mode (NPU and iGPU together) via OnnxRuntime-GenAI
using the oga-cpu recipe, and then use a thread to generate a streaming the
response to a prompt.

Note: this approach only works with recipes that support lemonade's OrtGenaiStreamer,
i.e., OGA-based recipes such as oga-cpu, oga-igpu, oga-npu, and oga-hybrid.

Make sure you have set up your OGA device in your Python environment.
See for details:
https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/README.md#install
"""

from threading import Thread
from lemonade.api import from_pretrained
from lemonade.tools.ort_genai.oga import OrtGenaiStreamer

model, tokenizer = from_pretrained(
    "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid", recipe="oga-hybrid"
)

input_ids = tokenizer("This is my prompt", return_tensors="pt").input_ids

streamer = OrtGenaiStreamer(tokenizer)
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
