# Lemonade Examples

This folder contains examples of how to use `lemonade` via the high-level LEAP APIs. These APIs make it easy to load a model, generate responses, and also show how to stream those responses.

The `demos/` folder also contains some higher-level application demos of the LEAP APIs. Learn more in `demos/README.md`.

## LEAP Examples

This table shows which LEAP examples are available:

| Framework                  | CPU                       | GPU              | NPU             | Hybrid             |
|----------------------------|---------------------------|------------------|-----------------|--------------------|
| Huggingface                | leap_basic.py             | -                | -               | -                  |
| OGA                        | leap_oga_cpu.py           | leap_oga_igpu.py | leap_oga_npu.py | leap_oga_hybrid.py |
| Huggingface with streaming | leap_streaming.py         | -                | -               | -                  |
| OGA with streaming         | leap_oga_cpu_streaming.py | leap_oga_igpu_streaming.py | leap_oga_npu_streaming.py | leap_oga_hybrid_streaming.py |

To run a LEAP example, first set up a conda environment with the appropriate framework and backend support. Then run the scripts with a command like `python leap_basic.py`.