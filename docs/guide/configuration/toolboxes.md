# Container Toolbox Backends

Three Lemonade backends run inside OCI container images instead of downloaded binaries:

| Recipe | Engine | Hardware |
|--------|--------|----------|
| `llamacpp-toolbox` | Forked llama.cpp builds mainline cannot substitute for | Strix Halo (gfx1151) |
| `ds4` | antirez's DwarfStar4 for the DeepSeek V4 family | Strix Halo, Radeon AI PRO R9700 |
| `halogen` | Peonist's Halogen Flash engine | Strix Halo only |

The images come from [Donato Capitella's toolbox project](https://github.com/kyuz0/amd-strix-halo-toolboxes)
and, for Halogen, from [Peonist](https://github.com/peonist-ai). They bundle a complete ROCm or
Vulkan stack, which is how they run engine builds that a plain binary release cannot deliver for
this hardware.

They are Linux-only by construction. On Windows and macOS these recipes are hidden.

## Prerequisites

You need a container runtime. Podman is preferred - it is rootless by default and passes your
group membership through to the container - but Docker works too. Everything else (pulling,
updating and removing images, wiring the GPU through, mounting models) is Lemonade's job.

If a backend shows **action required** in the backend manager, the message names the problem and
links to the matching section of the
[container prerequisites page](https://lemonade-server.ai/container_prerequisites.html).

## `llamacpp-toolbox` variants

Pick one with `--llamacpp-toolbox <variant>`, or set `backend` in the `toolbox` section of
`config.json`. `auto` picks the first variant your GPU supports.

| Variant | Image tag | What it is |
|---------|-----------|------------|
| `rocmfpx` | `rocm-10.0-rocmfpx` | ROCmFPX fork: FP3/FP4/FP6/FP8 weight formats, MTP |
| `nathanw` | `vulkan-radv-performance` | Nathan W's Vulkan performance fork |

Both are gfx1151-only upstream. These are the builds stock llama.cpp cannot stand in for: running
an ordinary GGUF on ROCm or Vulkan is what the existing `llamacpp` recipe already does, so this
recipe deliberately does not duplicate it.

`rocmfpx` exists because the weight formats it adds cannot be read by anything else - a model
quantized to ROCmFP4 will not load under `llamacpp`. `nathanw` takes ordinary GGUFs; it is a
tuning fork, not a new weight format.

kyuz0 also publishes a Vulkan build of the ROCmFPX fork, which is the only ROCmFPX image that
covers the Radeon AI PRO R9700 (gfx1201). Lemonade does not ship it: on Strix Halo it is redundant
with the HIP build above, and nothing here is validated on an R9700.

## Pinning and updates

Each variant is pinned by **digest**, not by tag, in `backend_versions.json`. The upstream tags are
rebuilt whenever their upstream moves - often daily - so a tag alone would mean two installs of
the same Lemonade release running different code. The tag is recorded next to the digest for
readability only.

A consequence worth knowing: a pinned image trails upstream by up to a week plus review time.
Digests move through a scheduled PR (`.github/workflows/toolbox_image_refresh.yml`), never
automatically.

`lemonade backends install llamacpp-toolbox:rocmfpx` pulls the pinned digest;
`lemonade backends uninstall llamacpp-toolbox:rocmfpx` removes the image. The "installed version"
Lemonade reports for these backends is the digest actually present on your machine.

## Options

```json
{
  "toolbox": {
    "backend": "auto",
    "args": ""
  }
}
```

- `backend` - which variant to run. `auto` picks the first supported one.
- `args` (`--toolbox-args`) - extra arguments for the containerized `llama-server`. Lemonade owns
  the model path, host, port, context size, `--jinja` and `--metrics`; passing your own copy of
  those is rejected.

Per variant, Lemonade applies the calibrated batch and micro-batch sizes and flash attention from
the upstream catalog's serving configs, and disables mmap for the ROCmFPX variants, whose weight
formats are decoded on the fly. Anything you pass in `args` wins, because llama.cpp parses left
to right.

## DS4

`ds4` runs the DeepSeek V4 family through antirez's DwarfStar4 engine, built from Donato's
performance branch. The published models are 80 GB and larger, so Lemonade launches it with three
settings a user would otherwise have to discover:

| Flag | Why |
|------|-----|
| `--ssd-streaming` | Reads experts from disk instead of making them resident. The only way an 80 GB model fits. |
| `--prefill-chunk 2048` | Chunks the prefill graph. Unchunked, a long prompt does not merely fail: it faults the GPU and takes the container with it. |
| `--ssd-streaming-cache-experts <half the device pool>GB` | Caps the expert cache so a long prompt's prefill still has room. |

That last one is the subtle one. ds4-server sizes its expert cache from the whole device arena,
which on an APU is the GTT window, and then a long prompt's prefill asks for an expert span the
arena can no longer satisfy because free memory has fallen under ds4's own 16 GiB reserve.
Measured on a 61.3 GiB arena: ds4's own choice of a 40.9 GiB cache planned a 48.2 GiB footprint
and died on a 2.6k-token prompt, while half the arena planned 37.4 GiB and answered it. Lemonade
therefore caps the cache at half the pool it detects. The GB form of the flag also reserves two
full prefill layers, which is the headroom that was missing.

All three are ordinary defaults: ds4-server parses left to right, so anything you pass in
`--ds4-args` wins.

Per-model context and prefill settings from the upstream catalog ride along as `recipe_options`
on each model entry.

Expect single-digit tokens per second. The model is streaming from an SSD, and that is the trade
being made to run an 80 GB mixture-of-experts at all.

## Halogen

`halogen` runs one model family: Qwen3.8-Flash-Next, as a 115 GiB HGN checkpoint plus a small
overlay that selects a quality or speed profile, optionally with a vision tower. It is configured
entirely through `HALOGEN_*` environment variables rather than a command line, and it is closed
source.

It has two requirements the other backends do not:

- **Linux 7.0 or newer.** There is no workaround; the memory path is not backported.
- **About 121 GiB of free disk.** All four Halogen entries share one download: the checkpoint and
  tokenizer are common, and the overlays and vision tower are small.

The checkpoint itself does not have to fit in memory. Halogen maps it read-only and registers the
mapping with the GPU rather than copying it. What must fit in the GPU's own pool is the KV pool,
measured at 7.2 GiB for the 262144-position pool it settles on here; the 68 GiB of weights it
locks are host RAM. The server measures that budget at startup and lowers the pool itself when the
configured one will not fit, so Lemonade deliberately leaves `HALOGEN_KV_POOL_POSITIONS` unset.

Measured on a 128 GB Strix Halo with the carve-out minimized: 96.5 GiB held in all, listening 92
seconds after launch, and around 45 tokens per second, which is roughly three times what the
llama.cpp toolbox variants reach on the same machine.

One hardware note worth acting on: if your BIOS carves a fixed block of memory out for the iGPU,
Halogen does not need it. It reaches the same unified memory through GTT either way, and the
carve-out is taken before the kernel boots, so it comes straight out of the file cache the mapped
checkpoint reads through. Setting the UMA frame buffer to Auto or its minimum is upstream's
recommendation.

## These are reasoning models

Every model the ROCmFPX, DS4 and Halogen entries point at reasons before it answers. That is worth
knowing because of how it interacts with `max_tokens`: the reasoning is spent out of the same
budget, and it arrives as `reasoning_content` rather than `content`. Ask for 16 tokens and you can
get an empty `content`, a populated `reasoning_content`, and `finish_reason: length`.

This is ordinary behavior for a reasoning model rather than anything specific to these backends,
but the models Lemonade ships for the other recipes mostly do not reason, so it is easy to meet
here first. Give these models room, or turn reasoning off per request with
`chat_template_kwargs: {"enable_thinking": false}` where the engine supports it.

## Model list

| Models | Recipe | Notes |
|--------|--------|-------|
| ROCmFP4 / ROCmI4 quantizations of Qwen3.8-27B and Qwopus3.6-27B | `llamacpp-toolbox` | Community conversions; the uploader is named in each entry |
| DeepSeek V4 Flash, DeepSeek V4.1 Flash, GLM 5.3 Flash | `ds4` | Translated from the upstream catalog |
| Qwen3.8-Flash-Next W4B, four overlay/vision combinations | `halogen` | All four share one checkpoint download |

The DS4 and Halogen entries are generated from the upstream catalog by
`docs/tools/gen_toolbox_catalog.py` and carry `catalog_source: ai-toolbox-cockpit`. The ROCmFPX
picks are curated by hand - one quantization per model family - because choosing among a
community uploader's variants is a judgment call, not a mapping.

## Limitations

- None of this works from inside Lemonade's own Docker image: a container cannot launch sibling
  containers without privileges that image does not request.
- Neither the toolbox repository nor the cockpit carries a license file, and Halogen is closed
  source. Lemonade pulls public images and translates public catalog data; it vendors no code.
- Donato's DS4 build tracks his performance branch, which may drift from antirez's main.
