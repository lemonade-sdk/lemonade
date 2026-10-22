# AMICA Backend Options

The `amica` recipe runs AMICA ChatApp model bundles (precompiled CGC MLIR graphs plus safetensors weights) on AMD GPUs through the public AMICA API and DirectX 12. It serves `chat/completions` and `completions`, streaming included.

For a step-by-step setup, including building AMICA and Lemonade and testing the result, see [`src/cpp/amica_engine/README.md`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/amica_engine/README.md).

## How it runs

A loaded model is served by **`amica-engine`**, a native program built with Lemonade against the AMICA API (`amica.h`, `amica.dll`). No Python is involved.

- When the model loads, it compiles the bundle's graphs, creates their pipeline state objects, and uploads the weights once. Weight tensors of 1 MiB or more are kept out of the compiled graphs and uploaded into one buffer each that prefill and decoder share, so a model's weights are in memory once rather than once per graph.
- It serves the OpenAI-compatible API Lemonade forwards to. Each request is rendered with the bundle's Jinja chat template (through [minja](https://github.com/google/minja)) and tokenized with the bundle's `tokenizer.json`, matching the AMICA ChatApp frontend. The tokenizer supports the byte-level BPE tokenizers ChatApp bundles use (Qwen, Llama 3, gpt-oss); a `tokenizer.json` that needs anything else is rejected when the model loads.
- Each request uploads only the prompt, runs prefill and then one decoder step per token, and reads back one row of logits per token. Tokens stream back as text as they are generated. For gpt-oss's Harmony format, the analysis channel is returned as `reasoning_content` and the final channel as `content`.

Generation is greedy: `temperature`, `top_p`, and the other sampling parameters are ignored. Requests run one at a time.

Prompt and context length are fixed by the bundle's graphs: the largest prefill graph sets the prompt capacity (128 tokens for Qwen2.5-0.5B), and the KV cache sets prompt plus generated tokens (192). Older turns are dropped to fit a conversation into the prefill capacity, and `max_tokens` is clamped to the remaining KV capacity.

Both bundle layouts that `amica-driver` runs are supported:

- **LM-head bundles** (such as Qwen2.5-0.5B, Qwen3-0.6B, and Llama 3.2 1B): prefill and decoder take FP32 embedding rows, and a separate LM-head graph produces logits.
- **Self-contained bundles** (`self_contained_logits`, such as gpt-oss-20b and qwen3.8-flash-next): prefill and decoder take token IDs and an attention mask and produce logits themselves. The context length is the decoder's attention-mask length.

Bundles that need `amica_optimizations` or `parameter_resources` are rejected when the model loads, because the public AMICA API cannot apply them.

If AMICA's execution planner rejects a graph, the model fails to load with the planner's reason. AMICA builds whose `amdgpu.rope_gqa` decode fusion does not declare its KV caches writable reject the qwen3.8-flash-next decoder this way (`graph output 9 has no binding`); use an AMICA build that includes that fix.

## Weight memory

By default every external weight is placed in GPU memory. `--weights-vram-mib N` (through `amica_args`) places weights in GPU memory smallest first up to N MiB, and the rest in system memory that the GPU reads directly, so a model larger than VRAM can still load:

```bash
lemonade load user.GPT-OSS-20B-AMICA --amica-args "--weights-vram-mib 4096"
```

On a discrete GPU, weights in system memory are read over PCIe on every token. On an RX 9070 XT, gpt-oss-20b with about 4 GiB of its 12 GiB of weights in VRAM generates 4.4 tokens/s instead of 122, with a 21 s TTFT, so use this only to make a model fit. qwen3.8-flash-next (about 100 GiB of weights) runs on a 16 GB RX 9070 XT with 128 GB of RAM using `--weights-vram-mib 12000`: 11.7 GiB of weights go to VRAM and 88.4 GiB to system memory, loading takes about 3 minutes, and it generates 3.7 tokens/s with a 23 s TTFT for a 57-token prompt. Weights in system memory stay allocated while the model is loaded, so the machine needs that much RAM free. On a unified-memory APU, both placements are the same physical memory; the option there only chooses the D3D12 heap the weights are allocated from.

## Requirements

- Windows 10 version 1903 or later (for the system ICU library) with an AMD RDNA3 or RDNA4 GPU.
- An AMICA install: a `tools` directory containing `amica-driver.exe`, next to the `amica/` directory containing `amica.dll`.
- An AMICA ChatApp model bundle: the CGC MLIR graphs, the safetensors weights, `tokenizer.json`, and the model's config files. A bundle needs no preparation: when it has no `amica_app.json`, `amica-engine` derives that manifest from the bundle's files the way the AMICA ChatApp does.

## Build

AMICA is not downloaded by Lemonade, so `amica-engine` is built only when CMake is given an AMICA package (the install directory that holds `include/amica`, `lib/amica.lib`, and `amica/`):

```bash
cmake --preset vs18 -DLEMON_AMICA_ROOT="C:/path/to/amica/install"
cmake --build --preset vs18
```

`amica-engine.exe` is placed next to `lemond.exe`. At run time it loads `amica.dll` from the install that `amica.dx12_bin` points at.

## Setup

Point Lemonade at the AMICA tools directory:

```bash
lemonade config set amica.dx12_bin="C:\path\to\amica\install\tools"
```

AMICA bundles are local directories rather than registry downloads. Register one in `user_models.json` in the Lemonade config directory with `"source": "local_path"` and the bundle directory as the checkpoint, then restart `lemond`:

```json
{
    "Qwen2.5-0.5B-Instruct-AMICA": {
        "source": "local_path",
        "checkpoint": "C:\\path\\to\\test\\amica\\mlir\\models\\qwen2.5-0.5b-cgc",
        "recipe": "amica",
        "labels": ["chat"],
        "size": 0.76
    }
}
```

Then load and chat with it:

```bash
lemonade run user.Qwen2.5-0.5B-Instruct-AMICA
```

## Options

| Option | Config key | Description |
|--------|------------|-------------|
| `amica_args` | `amica.args` | Extra `amica-engine` arguments: `--adapter <index or name>` selects the GPU (an index among AMD adapters, or part of the adapter name), `--arch <gfx...>` overrides the GPU architecture AMICA compiles for, and `--weights-vram-mib N` limits the weights placed in GPU memory (see [Weight memory](#weight-memory)). By default the engine uses the first supported discrete AMD GPU. |
| — | `amica.dx12_bin` | The AMICA `tools` directory, or the path to `amica-driver.exe` in it. |
