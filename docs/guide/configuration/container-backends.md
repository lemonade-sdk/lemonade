# Container Backends

A container backend runs its server from a container image instead of a downloaded binary. `lemonade backends` lists these backends with the format `container`. Installing one pulls its image, pinned by digest, and loading a model starts a container from that image.

Container backends run on Linux with Podman or Docker. Lemonade uses Podman when it is installed, and Docker otherwise.

| Backend | Engine | Image |
|---------|--------|-------|
| `ds4:rocm` | DS4 | `docker.io/kyuz0/strix-halo-ds4-toolbox` |
| `llamacpp:nathanw` | llama.cpp Vulkan performance fork | `docker.io/kyuz0/amd-strix-halo-toolboxes` |

## Setup

When `lemond` runs under your own account, from a shell, a `systemctl --user` unit, or an app that embeds it, Podman runs containers as that account. Set `CONTAINER_HOST` to root's Podman socket to run them with rootful Podman instead.

**With Podman:**

1. Install Podman, for example with `sudo apt install podman`, `sudo dnf install podman` or `sudo pacman -S podman`.
2. Add your account to `video` and `render`: `sudo usermod -aG video,render $USER`.
3. Log out and back in.

**With Docker:**

1. Add your account to the `docker` group: `sudo usermod -aG docker $USER`.
2. Log out and back in.

**With SELinux enforcing**, as on Fedora, ROCm backends also need containers allowed to map GPU devices: `sudo setsebool -P container_use_devices 1`.

Until setup is complete, the backend's state is `action_required`. `lemonade backends` prints the missing step and the commands that complete it, and the Backend Manager shows the same. The state updates as soon as the step takes effect.

## Install and Run

```bash
lemonade backends install ds4:rocm
lemonade run DeepSeek-V4-Flash-Vision-IQ2XXS-DS4
```

Install pulls the image, so loading a model starts from the image already on disk. `lemonade backends uninstall ds4:rocm` removes it.

## How Containers Run

Each loaded model gets its own container, named `lemonade-<recipe>-<backend>-<model>` and labeled `ai.lemonade`, so `podman ps --filter label=ai.lemonade` lists them. Every load logs its full `podman run` or `docker run` command. Each container:

- mounts only the model's files, read-only, under `/mnt/models`, with the shared SELinux label so other containers and programs can still read them
- opens only the GPU device nodes its backend needs
- drops every Linux capability its backend does not need, and runs with `no-new-privileges`
- joins its own `--internal` network, so only this machine reaches its API and the server inside stays off the internet

Unloading a model stops its container. When `lemond` starts, it removes any `ai.lemonade` containers and networks a previous run left behind.
