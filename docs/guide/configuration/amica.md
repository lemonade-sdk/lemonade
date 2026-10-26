# AMICA Backend Options

The `amica` recipe runs AMICA ChatApp model bundles (precompiled CGC MLIR graphs plus safetensors weights) on AMD GPUs through the public AMICA API and DirectX 12. It serves `chat/completions` and `completions`, streaming included. The `amica-image` recipe runs AMICA GenaiApp image bundles (FLUX.1-schnell) the same way and serves `images/generations`; see [Image generation](#image-generation).

For a step-by-step setup, including building AMICA and Lemonade and testing the result, see [`src/cpp/amica_engine/README.md`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/amica_engine/README.md).

## How it runs

A loaded model is served by **`amica-engine`**, a native program built with Lemonade against the AMICA API (`amica.h`, `amica.dll`). No Python is involved.

- When the model loads, it compiles the bundle's graphs, creates their pipeline state objects, and uploads the weights once. Weight tensors of 1 MiB or more are kept out of the compiled graphs and uploaded into one buffer each that prefill and decoder share, so a model's weights are in memory once rather than once per graph.
- It serves the OpenAI-compatible API Lemonade forwards to. Each request is rendered with the bundle's Jinja chat template (through [minja](https://github.com/google/minja)) and tokenized with the bundle's `tokenizer.json`, matching the AMICA ChatApp frontend. The tokenizer supports the byte-level BPE tokenizers ChatApp bundles use (Qwen, Llama 3, gpt-oss); a `tokenizer.json` that needs anything else is rejected when the model loads.
- Each request uploads only the prompt, runs prefill and then one decoder step per token, and reads back one row of logits per token. Tokens stream back as text as they are generated. Reasoning is returned separately as `reasoning_content`, with the answer in `content`: for gpt-oss's Harmony format the analysis channel is the reasoning, for Muse Glimmer the messages it addresses `to=self` are, and for models that think between `<think>` and `</think>` (such as qwen3.8-flash-next and Qwen3) that text is.

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

## Image generation

The `amica-image` recipe serves an AMICA GenaiApp image bundle: the directory `GenaiApp.ps1 --model flux-schnell` prepares under `test\amica\mlir\models\flux-schnell`, with `cgc_pipeline.json`, a CGC graph and weights for each component, and the source model's ONNX tokenizers and scheduler config under `onnx\`. FLUX.1-schnell is the supported model. Lemonade starts the same `amica-engine`, which recognizes an image bundle by its `cgc_pipeline.json`.

- When the model loads, the engine compiles the CLIP and T5 text encoders, the transformer and the VAE decoder, plus three small graphs it generates for the work between them (packing the noise into the transformer's latent layout, each flow-matching scheduler step, and unpacking the latent for the VAE). The VAE encoder is not loaded.
- Prompts are tokenized natively from the vocabulary, merges and SentencePiece model inside the bundle's ONNX tokenizer files, producing the same token IDs as the ONNX Runtime extensions GenaiApp runs.
- A request runs every graph in one GPU submission and reads back only the finished image, which is returned as a base64 PNG in `b64_json`.

Request fields:

- `prompt` (required).
- `steps` (default 4, at most 50). FLUX.1-schnell is distilled for 1 to 4 steps.
- `seed`. An absent or negative seed is random. The same seed gives the same image, but not the image GenaiApp gives for that seed, because the noise generator differs.
- `n` (default 1, at most 10). Images after the first use seeds `seed+1`, `seed+2`, and so on.
- `size` or `width`/`height`. The size is fixed when GenaiApp prepares the bundle (256x256 by default), and other sizes are rejected.

Guidance (`cfg_scale`) does not apply to FLUX.1-schnell and is ignored. `images/edits` and `images/variations` are not supported.

Unless `--weights-vram-mib` is given, the image engine places weights in VRAM up to the memory the OS grants it, less what its own buffers need, and the rest in system memory. FLUX.1-schnell's FP16 weights are about 25 GiB, so on a 16 GB RX 9070 XT about 13.7 GiB go to VRAM and 11.3 GiB to system memory, and an image takes about 5 minutes because each transformer step reads the offloaded weights over PCIe.

`--transformer-precision int4-weight-only` (through `amica_args`) loads the bundle's INT4 weight-only transformer from `transformer\int4-weight-only` instead. Its weights are 5.8 GiB, so the whole model (8.4 GiB) fits in VRAM: on the RX 9070 XT it loads in about 12 s and generates a 4-step 256x256 image in about 9 s. AMICA marks this variant unqualified: its images are close to the FP16 transformer's but not identical. GenaiApp does not download its weights by default.

Register the bundle with `image_defaults` that match its size, so clients such as the Lemonade app request that size:

```json
{
    "FLUX.1-schnell-AMICA": {
        "source": "local_path",
        "checkpoint": "C:\\path\\to\\test\\amica\\mlir\\models\\flux-schnell",
        "recipe": "amica-image",
        "labels": ["image"],
        "image_defaults": {"steps": 4, "width": 256, "height": 256},
        "recipe_options": {"amica_args": "--transformer-precision int4-weight-only"}
    }
}
```

Leave out `recipe_options` to use the FP16 transformer.

`amica-image` uses the `amica` section of `config.json`: the same `amica.dx12_bin` and `amica.args`.

## Requirements

- Windows 10 version 1903 or later (for the system ICU library) with an AMD RDNA3 or RDNA4 GPU.
- An AMICA install: a `tools` directory containing `amica-driver.exe`, next to the `amica/` directory containing `amica.dll`.
- An AMICA ChatApp model bundle: the CGC MLIR graphs, the safetensors weights, `tokenizer.json`, and the model's config files. A bundle needs no preparation: when it has no `amica_app.json`, `amica-engine` derives that manifest from the bundle's files the way the AMICA ChatApp does.
- For `amica-image`, a GenaiApp image bundle that GenaiApp has prepared. The bundle must contain each component's `source_fp16act.mlir` and `weights.safetensors`.

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
