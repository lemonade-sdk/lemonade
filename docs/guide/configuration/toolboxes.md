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
performance branch. The published models are 80 GB and larger, so DS4 always launches with
`--ssd-streaming`: the experts are read from disk instead of being made fully resident, which is
the only way they fit in a Strix Halo memory carveout.

Per-model context and prefill settings come from the upstream catalog and ride along as
`recipe_options` on each model entry.

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
mapping with the GPU rather than copying it, so what must fit is the KV pool: about 28 GB for one
full native-context conversation, 35 GB for the default two. The server measures the budget at
startup and lowers the pool itself when the configured one will not fit, so Lemonade deliberately
leaves `HALOGEN_KV_POOL_POSITIONS` unset.

One hardware note worth acting on: if your BIOS carves a fixed block of memory out for the iGPU,
Halogen does not need it. It reaches the same unified memory through GTT either way, and the
carve-out is taken before the kernel boots, so it comes straight out of the file cache the mapped
checkpoint reads through. Setting the UMA frame buffer to Auto or its minimum is upstream's
recommendation.

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
