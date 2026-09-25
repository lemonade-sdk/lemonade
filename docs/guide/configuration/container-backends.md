# Container Backends

Some Lemonade backends run their server from a digest-pinned OCI image instead of a downloaded
binary. Lemonade starts each one with Podman or Docker on Linux, and everything else works as for
any other backend: install, load, unload, `/system-info`, benchmarking and the backend manager.

| Backend | Engine | Hardware |
|---------|--------|----------|
| `rocmfpx:rocmfpx` | ROCm FPX: a llama.cpp fork adding FP4/FP6/FP8 weights and MTP | Strix Halo (gfx1151) |
| `llamacpp:nathanw` | Nathan W's Vulkan performance build of llama.cpp | Strix Halo (gfx1151) |
| `ds4:rocm` | antirez's DwarfStar4 for the DeepSeek V4 family | Strix Halo, Radeon AI PRO R9700 |
| `halogen:rocm` | Peonist's Halogen Flash engine | Strix Halo (gfx1151) |

The rocmfpx, nathanw and DS4 images come from
[Donato Capitella's toolbox project](https://github.com/kyuz0/amd-strix-halo-toolboxes), and the
Halogen image from [Peonist](https://github.com/peonist-ai). They bundle a complete ROCm or Vulkan
stack, which is how they run engine builds that a plain binary release cannot deliver for this
hardware. Container backends are hidden on Windows and macOS.

- [Set up your machine](#set-up-your-machine)
- [Fix an action-required backend](#fix-an-action-required-backend)
- [Run Lemonade in a sandbox](#run-lemonade-in-a-sandbox)
- [What a container gets](#what-a-container-gets)
- [Choose between the two llama.cpp forks](#choose-between-the-two-llamacpp-forks)
- [Pinning](#pinning)
- [Options](#options)
- [DS4](#ds4)
- [Halogen](#halogen)
- [Reasoning models](#reasoning-models)
- [Model list](#model-list)
- [Limitations](#limitations)

## Set up your machine

A container backend loads when `lemond` can reach Podman or Docker, and that tool can open the
GPU's device nodes. When both are installed, Lemonade uses Podman. Docker here means the standard
Docker daemon, which runs as root.

What you do depends on what starts `lemond`. With Podman:

| What starts `lemond` | What the package does | What you do |
|----------------------|-----------------------|-------------|
| `lemond.service` from the .deb or .rpm | Installs and enables `lemonade-podman.socket` | Install Podman |
| `lemond.service` from the Arch `lemonade-server` package | Installs `lemonade-podman.socket` | Install Podman, then run `sudo systemctl enable --now lemonade-podman.socket` |
| You, as your own account: a `systemctl --user` unit, a shell, a `lemond` built from source, or an app that embeds `lemond` | Nothing | Install Podman, add your account to `video` and `render` if it is not in both, then log out and back in |
| The `lemonade-server` snap | Ships the `podman` CLI and plug | Install Podman from your distribution, run `sudo systemctl enable --now podman.socket`, then `sudo snap connect lemonade-server:podman :podman` |
| The Lemonade Docker image | Ships the `podman` CLI and sets `CONTAINER_HOST` | Follow [Docker: Container Backends](../install/docker.md#container-backends) |
| You, inside a toolbox or distrobox | Nothing | Install Podman on the host, add your host account to `video` and `render` if it is not in both, then log out and back in |

With Docker, the daemon opens the device nodes itself, so no account needs `video` or `render`.
Membership in the `docker` group is equivalent to root, so no package adds an account to it:

| What starts `lemond` | What you do |
|----------------------|-------------|
| `lemond.service` from the .deb, .rpm or Arch package | Install Docker, run `sudo usermod -aG docker lemonade`, then `sudo systemctl restart lemond` |
| You, as your own account | Install Docker, run `sudo usermod -aG docker $USER`, then log out and back in |
| The `lemonade-server` snap | Install the `docker` snap, then run `sudo snap connect lemonade-server:docker docker:docker-daemon` |
| The Lemonade Docker image | Follow [Docker: Container Backends](../install/docker.md#container-backends) |
| You, inside a toolbox or distrobox | Install Docker on the host, add your host account to the `docker` group, then log out and back in |

### The system service and Podman

`lemond.service` runs as the `lemonade` account with `RestrictNamespaces=yes` and
`NoNewPrivileges=yes`. Rootless Podman needs both user namespaces and the setuid `newuidmap`
helper, so Podman runs in a separate, socket-activated service instead:

- `lemonade-podman.socket` listens on `/run/lemonade-podman.sock`, which only `lemonade` can open.
- `lemonade-podman.service` runs `podman system service` as `lemonade`. It starts on the first
  connection and exits after 60 seconds idle.
- `lemond.service` sets `CONTAINER_HOST=unix:///run/lemonade-podman.sock`, so every `podman`
  command runs with `--remote` and its containers start from `lemonade-podman.service`.
- The service checks for `/usr/bin/podman` at each start, so Podman installed after Lemonade works
  without restarting either service.
- On its first start, the service gives `lemonade` the subordinate uid and gid range
  `200000-265535`, which rootless Podman needs.
- The packages' `sysusers.d/lemonade.conf` adds `lemonade` to `video` and `render`.

A compromised `lemond` can have `lemonade-podman.service` start containers with the rights of the
`lemonade` account.

## Fix an action-required backend

While a setup step is missing, the backend's state is **action required**, with two fields:

- `message`: the check that failed.
- `action`: the commands that fix it, or, in the Lemonade Docker image, a link to
  [Docker: Container Backends](../install/docker.md#container-backends).

`/system-info` reports both. The backend manager in the desktop and web apps shows the message,
with a help button that copies the action, and `lemonade backends` prints both. The checks rerun on
every `/system-info` request, so the backend becomes installable as soon as the fix takes effect.

| Check | Fails when | Typical action |
|-------|------------|----------------|
| Podman installed | Neither `podman` nor `docker` is on `PATH` | `sudo apt install podman`, `sudo dnf install podman` or `sudo pacman -S podman`, chosen from `/etc/os-release` |
| Podman reachable | `/run/lemonade-podman.sock` does not answer (system service) | `sudo systemctl enable --now lemonade-podman.socket` |
| Device nodes accessible | The account running `lemond` is not in both `video` and `render` | `sudo usermod -aG video,render lemonade`, then `sudo systemctl restart lemond`; or `sudo usermod -aG video,render $USER`, then log out and back in |
| Docker reachable | The Docker daemon refuses the account running `lemond` | `sudo usermod -aG docker lemonade`, then `sudo systemctl restart lemond`; or `sudo usermod -aG docker $USER`, then log out and back in |

In the snap, the checks look at the `podman` and `docker` plugs and at `/run/podman/podman.sock`.
In the Lemonade Docker image, they look at the mounted sockets and whether `lemond` can inspect its
own container through them.

## Run Lemonade in a sandbox

When `lemond` runs inside the Lemonade Docker image, the `lemonade-server` snap or a toolbox, the
container tool runs on the host. Lemonade detects each case at startup and drives the host's tool:

| Where `lemond` runs | Detected by | How it reaches the tool | Setup |
|---------------------|-------------|-------------------------|-------|
| Lemonade Docker image | `/.dockerenv` or `/run/.containerenv` | The bundled `podman` or `docker` CLI, through the host socket you mount. Lemonade inspects its own container to find the host side of the model cache, and each backend container shares `lemond`'s network namespace. | [Docker: Container Backends](../install/docker.md#container-backends) |
| `lemonade-server` snap | `SNAP_NAME` | The bundled `podman` CLI through the `podman` plug to the host's root Podman at `/run/podman/podman.sock`, or the bundled `docker` CLI through the `docker` plug to the `docker` snap | `sudo snap connect lemonade-server:podman :podman` or `sudo snap connect lemonade-server:docker docker:docker-daemon` |
| Toolbox or distrobox | `/run/.toolboxenv` | `flatpak-spawn --host podman` or `flatpak-spawn --host docker`, with `/run/host` removed from model paths | None beyond the host setup above |

snapd never auto-connects the `podman` and `docker` plugs, so you connect them by hand. The
`podman` interface needs snapd 2.76 or newer.

## What a container gets

Lemonade builds the whole `run` command, and every load logs it in full. Every container backend
starts the same way:

| Option | Value |
|--------|-------|
| Image | The pinned `repository@sha256:<digest>` for your GPU's arch, with `--pull=never` |
| Isolation | `--rm --init --cap-drop=all --security-opt=no-new-privileges --security-opt=label=disable`, and the tool's default seccomp profile |
| Name | `lemonade-<recipe>-<backend>-<model>` |
| Labels | `ai.lemonade`, plus `ai.lemonade.recipe`, `.backend`, `.model` and `.port` |
| Network | A private `--internal` network per container, named after it |
| Devices | The entry's device nodes, passed whole: `/dev/dri`, plus `/dev/kfd` for ROCm images. With `/dev/kfd`, `HIP_VISIBLE_DEVICES` names the GPU that matches the image's arch. |
| Groups | Podman: `--group-add keep-groups`. Docker: the host's numeric `video` and `render` group IDs. |
| Model | Each model file or directory the engine needs, bind-mounted read-only under `/mnt/models` |
| Environment | `HOME=/tmp`, plus the variables the engine needs |
| Port | Podman: `-p 127.0.0.1:<port>:<port>`. Docker publishes no port from an internal network, so Lemonade connects to the container's address on it. |

Some engines need more, and only those get it:

| Backend | Adds |
|---------|------|
| `ds4:rocm` | `--ipc=host` and `--cap-add SYS_PTRACE`, which its expert streaming needs |
| `halogen:rocm` | `--ipc=host`, `--ulimit memlock=-1:-1`, and the engine settings in [Halogen](#halogen) |

The `podman run` or `docker run` client is a child of `lemond`, and the container's processes
belong to the tool's supervisor (`conmon` or `containerd-shim`). On unload, Lemonade runs
`stop --time 10` on the container by name, then removes the container and its network. When
`lemond` starts, it removes every container labeled `ai.lemonade`, including stopped ones, so a killed `lemond` never leaves a container holding the GPU.

## Choose between the two llama.cpp forks

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
`config.json`. Lemonade uses it only when you pick it.

## Pinning

Each image is pinned by **digest**, not by tag, in `backend_versions.json`. The upstream tags are
rebuilt whenever their upstream moves, often daily, so a tag alone would mean two installs of the
same Lemonade release running different code. The tag is recorded next to the digest for
readability only.

`lemonade backends install rocmfpx:rocmfpx` pulls the pinned digest, and
`lemonade backends uninstall rocmfpx:rocmfpx` removes the image. The same two commands work for
`llamacpp:nathanw`, `ds4:rocm` and `halogen:rocm`. A load never downloads an image.

## Options

```json
{
  "rocmfpx": {
    "args": ""
  }
}
```

- `args` (`--rocmfpx-args`): extra arguments for the containerized `llama-server`. Lemonade owns
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
| `--prefill-chunk 2048` | Chunks the prefill graph. Unchunked, a long prompt faults the GPU and takes the container with it. |
| `--ssd-streaming-cache-experts <half the device pool>GB` | Caps the expert cache so a long prompt's prefill still has room. |

ds4-server sizes its expert cache from the whole device arena, which on an APU is the GTT window,
and then a long prompt's prefill asks for an expert span the arena can no longer satisfy because
free memory has fallen under ds4's own 16 GiB reserve. Measured on a 61.3 GiB arena: ds4's own
choice of a 40.9 GiB cache planned a 48.2 GiB footprint and died on a 2.6k-token prompt, while half
the arena planned 37.4 GiB and answered it. Lemonade therefore caps the cache at half the pool it
detects. The GB form of the flag also reserves two full prefill layers.

All three are defaults: ds4-server parses left to right, so anything you pass in `--ds4-args`
wins. Per-model context and prefill settings from the upstream catalog ride along as
`recipe_options` on each model entry.

Expect single-digit tokens per second. The model streams from an SSD, which is the trade that lets
an 80 GB mixture-of-experts run at all.

## Halogen

`halogen` runs one model family: Qwen3.8-Flash-Next, as a 115 GiB HGN checkpoint plus a small
overlay that selects a quality or speed profile, optionally with a vision tower. It is configured
entirely through `HALOGEN_*` environment variables rather than a command line, and it is closed
source.

It has two requirements the other backends do not:

- **Linux 7.0 or newer.** Halogen's memory path depends on kernel support that is not backported.
- **About 121 GiB of free disk.** All four Halogen entries share one download: the checkpoint and
  tokenizer are common, and the overlays and vision tower are small.

Lemonade starts Halogen with AI Toolbox Cockpit's engine settings:

| Variable | Value |
|----------|-------|
| `HALOGEN_CTX` | `262144`, or the model's `ctx_size` when you set one |
| `HALOGEN_KV_POOL_POSITIONS` | `524288` |
| `HALOGEN_KV_SLOTS` | `4` |
| `HALOGEN_PROMPT_CACHE` | `2` |

The checkpoint does not have to fit in the GPU's pool. Halogen maps it read-only and registers the
mapping with the GPU rather than copying it.

If your BIOS carves a fixed block of memory out for the iGPU, Halogen does not need it. It reaches
the same unified memory through GTT either way, and the carve-out is taken before the kernel boots,
so it comes straight out of the file cache the mapped checkpoint reads through. Setting the UMA
frame buffer to Auto or its minimum is upstream's recommendation.

## Reasoning models

Every model the ROCm FPX, DS4 and Halogen entries point at reasons before it answers. The reasoning
is spent out of the same `max_tokens` budget, and it arrives as `reasoning_content` rather than
`content`. Ask for 16 tokens and you can get an empty `content`, a populated `reasoning_content`,
and `finish_reason: length`.

Give these models room, or turn reasoning off per request with
`chat_template_kwargs: {"enable_thinking": false}` where the engine supports it.

## Model list

| Models | Recipe | Notes |
|--------|--------|-------|
| ROCmFP4 / ROCmI4 quantizations of Qwen3.8-27B and Qwopus3.6-27B | `rocmfpx` | Community conversions; the uploader is named in each entry |
| DeepSeek V4 Flash, DeepSeek V4.1 Flash, GLM 5.3 Flash | `ds4` | Taken from the AI Toolbox Cockpit catalog |
| Qwen3.8-Flash-Next W4B, four overlay/vision combinations | `halogen` | All four share one checkpoint download |

The ROCm FPX picks are curated by hand, one quantization per model family, because choosing among
a community uploader's variants is a judgment call, not a mapping. `nathanw` has no models of its
own: it runs whatever the `llamacpp` recipe already lists.

## Limitations

- Linux only, one AMD GPU.
- Neither the toolbox repository nor AI Toolbox Cockpit carries a license file, and Halogen is
  closed source. Lemonade pulls public images and translates public catalog data; it vendors no
  code.
- Donato's DS4 build tracks his performance branch, which may drift from antirez's main.
