# Container Backends

Some Lemonade backends run inside OCI container images instead of downloaded binaries:

| Recipe | Backend | Engine | Hardware |
|--------|---------|--------|----------|
| `rocmfpx` | `rocmfpx` | ROCm FPX: a llama.cpp fork adding FP4/FP6/FP8 weights and MTP | Strix Halo (gfx1151) |
| `llamacpp` | `nathanw` | Nathan W's Vulkan performance build of llama.cpp | Strix Halo (gfx1151) |
| `ds4` | `ds4` | antirez's DwarfStar4 for the DeepSeek V4 family | Strix Halo, Radeon AI PRO R9700 |
| `halogen` | `halogen` | Peonist's Halogen Flash engine | Strix Halo only |

The images come from [Donato Capitella's toolbox project](https://github.com/kyuz0/amd-strix-halo-toolboxes)
and, for Halogen, from [Peonist](https://github.com/peonist-ai). They bundle a complete ROCm or
Vulkan stack, which is how they run engine builds that a plain binary release cannot deliver for
this hardware. Which collection an image is published in is packaging, so it does not appear in
Lemonade: what you pick is an engine, and for `nathanw`, a build of one.

They are Linux-only by construction. On Windows and macOS these backends are hidden.

## Prerequisites

You need a container runtime. Podman is preferred - it is rootless by default and passes your
group membership through to the container - but Docker works too. Everything else (pulling,
updating and removing images, wiring the GPU through, mounting models) is Lemonade's job.

If a backend shows **action required** in the backend manager, the message names the problem and
links to the matching section of the
[container prerequisites page](https://lemonade-server.ai/container_prerequisites.html).

## What a container gets

Lemonade owns the whole `run` command line. Every toolbox container is started the same way:

| Argument | Value |
|----------|-------|
| Image | The pinned `repository@sha256:<digest>`, with `--pull=never` |
| Isolation | `--rm --init --cap-drop=all --security-opt=no-new-privileges --security-opt=label=disable` |
| Network | A private `--internal` network per container, named after it: no default route, no NAT, nothing else on it |
| Ownership | `--label ai.lemonade`, plus the recipe, backend and port as labels. Sweep and stop find containers by that label. |
| Devices | `/dev/kfd` and `/dev/dri`, passed whole; `HIP_VISIBLE_DEVICES` names the GPU whose ISA the image is pinned for |
| Groups | The host's `video` and `render` gids (`keep-groups` on rootless podman) |
| Model | Each model file, resolved through the Hugging Face cache's symlinks, bind-mounted read-only at `/mnt/models/<file>` |
| Port | Podman: `-p 127.0.0.1:<port>:<port>`. Docker cannot publish from an internal network, so Lemonade connects to the container's address on it. Either way the engine is reachable only from this machine, and Lemonade proxies. |

DS4 additionally gets `--ipc=host` and `SYS_PTRACE`, which its expert streaming needs. Nothing
else is added: the container sees exactly the model files it was given, has no network, and
cannot gain privileges. The user's only input is extra engine flags.

The container is not a child of `lemond`. Its processes belong to the container engine's
supervisor (`conmon` for podman, `containerd-shim` for Docker). `lemond` holds an attached
`podman run` client with signal proxying, stops the container by name on unload, and removes
every container carrying the `ai.lemonade` label when it starts, so a killed `lemond` never leaves
a container holding the GPU. If a load times out, the error carries the container's last log
lines.

## Running Lemonade in a sandbox

When `lemond` itself runs inside the Lemonade Docker image, a strictly confined snap or a Fedora
Toolbox, the container engine lives outside it. Lemonade detects each case and drives the host's
engine through it:

| Where `lemond` runs | How it reaches the engine | One-time setup |
|---------------------|---------------------------|----------------|
| Docker image | The engine socket mounted into the container; toolboxes join Lemonade's network namespace | Add the socket mount to `docker run` ([Docker guide](../install/docker.md#toolbox-backends-from-the-docker-image)) |
| Snap | snapd's `docker` interface to the Docker snap's daemon | `snap connect lemonade:docker docker:docker-daemon` |
| Toolbox / Distrobox | `flatpak-spawn --host podman` | None |

Inside the Docker image Lemonade inspects its own container to find the host side of the model
cache, so no paths are configured.

## The system service and rootless podman

`lemond.service` runs as the `lemonade` user. Rootless podman needs subordinate uid/gid ranges
for that user, a lingering user session, and user namespaces, which the unit's hardening would
otherwise forbid. The .deb and .rpm packages grant the ranges and enable linger at install time
and ship a drop-in (`lemond.service.d/containers.conf`) that allows the namespaces. If the unit
was edited by hand, the backend manager's action-required message names the missing piece.

## Why these two llama.cpp forks sit in different places

Both are forks of llama.cpp shipped from the same image collection, and they are reached
differently because they differ in kind.

**`rocmfpx` is its own engine.** It adds FP4/FP6/FP8 weight formats that nothing else can read: a
model quantized to ROCmFP4 will not load under `llamacpp`, and an ordinary GGUF gains nothing from
it. Its models name their format (`Qwen3.8-27B-ROCmFP4-FAST`), and they carry the `rocmfpx` recipe.
There is one build, so there is nothing to select; pass extra llama-server flags with
`--rocmfpx-args`, or the `args` key of the `rocmfpx` section of `config.json`.

**`nathanw` is a build of `llamacpp`.** It reads ordinary GGUFs and only changes how fast they run,
so it is a backend of the llama.cpp recipe alongside `vulkan` and `rocm`. Any `llamacpp` model can
use it: pick it with `--llamacpp nathanw`, or set `backend` in the `llamacpp` section of
`config.json`. It is never chosen automatically, including when it is the only backend installed,
because an experimental fork should be something you asked for.

kyuz0 also publishes a Vulkan build of the ROCm FPX fork, which is the only one that covers the
Radeon AI PRO R9700 (gfx1201). Lemonade does not ship it: on Strix Halo it is redundant with the
HIP build, and nothing here is validated on an R9700.

## Pinning and updates

Each image is pinned by **digest**, not by tag, in `backend_versions.json`. The upstream tags are
rebuilt whenever their upstream moves - often daily - so a tag alone would mean two installs of
the same Lemonade release running different code. The tag is recorded next to the digest for
readability only.

A consequence worth knowing: a pinned image trails upstream by up to a week plus review time.
Digests move through a scheduled PR (`.github/workflows/toolbox_refresh.yml`), never
automatically.

`lemonade backends install rocmfpx:rocmfpx` pulls the pinned digest;
`lemonade backends uninstall rocmfpx:rocmfpx` removes the image. The same two commands work for
`llamacpp:nathanw`. The "installed version" Lemonade reports for these backends is the digest
actually present on your machine.

## Options

```json
{
  "rocmfpx": {
    "args": ""
  }
}
```

- `args` (`--rocmfpx-args`) - extra arguments for the containerized `llama-server`. Lemonade owns
  the model path, host, port, context size, `--jinja` and `--metrics`; passing your own copy of
  those is rejected. `nathanw` takes the same kind of arguments through `--llamacpp-args`.

Lemonade applies the calibrated batch and micro-batch sizes and flash attention from the upstream
catalog's serving configs, and disables mmap for ROCm FPX, whose weight formats are decoded on the
fly. Anything you pass in `args` wins, because llama.cpp parses left to right.

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

The context is left to the engine for the same reason: an HGN checkpoint carries none of the
architecture metadata Lemonade's auto-tuning reads, so on `ctx_size: -1` (the default) Halogen
starts at its native 262144 and fits the pool to the memory it measures. Setting `ctx_size`
explicitly overrides that, and Lemonade then lowers the server's default `max_tokens` to match,
because a request reserves prompt plus `max_tokens` against the context.

Measured on a 128 GB Strix Halo with the carve-out minimized: 96.5 GiB held in all, listening 92
seconds after launch, and around 45 tokens per second, which is roughly three times what the
the llama.cpp forks reach on the same machine.

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
| ROCmFP4 / ROCmI4 quantizations of Qwen3.8-27B and Qwopus3.6-27B | `rocmfpx` | Community conversions; the uploader is named in each entry |
| DeepSeek V4 Flash, DeepSeek V4.1 Flash, GLM 5.3 Flash | `ds4` | Translated from the upstream catalog |
| Qwen3.8-Flash-Next W4B, four overlay/vision combinations | `halogen` | All four share one checkpoint download |

The DS4 and Halogen entries are generated from the upstream catalog by
`docs/tools/gen_toolbox_catalog.py` and carry `catalog_source: ai-toolbox-cockpit`. The ROCm FPX
picks are curated by hand - one quantization per model family - because choosing among a
community uploader's variants is a judgment call, not a mapping. `nathanw` has no models of its
own: it runs whatever the `llamacpp` recipe already lists.

## Limitations

- None of this works from inside Lemonade's own Docker image: a container cannot launch sibling
  containers without privileges that image does not request.
- Neither the toolbox repository nor the cockpit carries a license file, and Halogen is closed
  source. Lemonade pulls public images and translates public catalog data; it vendors no code.
- Donato's DS4 build tracks his performance branch, which may drift from antirez's main.
