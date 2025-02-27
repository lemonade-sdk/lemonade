# Lemonade Examples

This folder contains examples of how to use `lemonade` via the high-level APIs. These APIs make it easy to load a model, generate responses, and also show how to stream those responses.

The `demos/` folder also contains some higher-level application demos of the APIs. Learn more in `demos/README.md`.

This table shows which API examples are available:

| Framework                  | CPU                       | GPU              | NPU             | Hybrid             |
|----------------------------|---------------------------|------------------|-----------------|--------------------|
| Huggingface                | api_basic.py              | -                | -               | -                  |
| OGA                        | api_oga_cpu.py            | api_oga_igpu.py | api_oga_npu.py | api_oga_hybrid.py |
| Huggingface with streaming | api_streaming.py          | -                | -               | -                  |
| OGA with streaming         | api_oga_cpu_streaming.py  | api_oga_igpu_streaming.py | api_oga_npu_streaming.py | api_oga_hybrid_streaming.py |

To run an API example, first set up a conda environment with the appropriate framework and backend support. Then run the scripts with a command like `python api_basic.py`.