# Container Backends

A container backend runs its server from a container image instead of a downloaded binary. `lemonade backends` lists these backends with the format `container`. Installing one pulls its image, pinned by digest, and loading a model starts a container from that image.

Container backends run on Linux with Podman or Docker. Lemonade picks one when it runs a container:

| Engine | Used when |
|--------|-----------|
| Rootless Podman (default) | Podman is installed. Containers run as the account `lemond` runs under. |
| Rootful Podman | Podman is installed and `CONTAINER_HOST` points at a root Podman socket, or `lemond` runs as root. |
| Docker | Podman is not installed. |

Rootful Podman and Docker give that account root-equivalent control of the host's containers.

| Backend | Engine | Image |
|---------|--------|-------|
| `llamacpp:nathanw` | llama.cpp Vulkan performance fork | [kyuz0/amd-strix-halo-toolboxes](https://hub.docker.com/r/kyuz0/amd-strix-halo-toolboxes) |

## Setup

When `lemond` runs under your own account, from a shell, a `systemctl --user` unit, or an app that embeds it, its containers sit beside your own in `podman ps`, and it uses whatever `CONTAINER_HOST` your environment sets.

**With Podman:**

1. Install Podman, for example with `sudo apt install podman`, `sudo dnf install podman` or `sudo pacman -S podman`.
2. Add your account to `video` and `render`: `sudo usermod -aG video,render $USER`.
3. Log out and back in.

**With Docker:**

1. Add your account to the `docker` group: `sudo usermod -aG docker $USER`.
2. Log out and back in.

When `lemond` runs as `lemond.service`, installed by the `.deb`, PPA or `.rpm` package, it runs under the `lemonade` account. The package pulls in Podman, adds `lemonade` to `video` and `render`, and allocates its subordinate UID and GID range, so rootless Podman needs no further setup.

**`lemond.service` with rootful Podman:** give `lemonade` its own root Podman socket, leaving root's `podman.socket` as it is.

1. Create `/etc/systemd/system/podman-lemonade.socket`:
   ```ini
   [Socket]
   ListenStream=/run/podman-lemonade.sock
   SocketGroup=lemonade
   SocketMode=0660

   [Install]
   WantedBy=sockets.target
   ```
2. Create `/etc/systemd/system/podman-lemonade.service`:
   ```ini
   [Service]
   Delegate=true
   Type=exec
   KillMode=process
   ExecStart=/usr/bin/podman system service
   ```
3. `sudo systemctl daemon-reload && sudo systemctl enable --now podman-lemonade.socket`
4. Add `CONTAINER_HOST=unix:///run/podman-lemonade.sock` to `/etc/default/lemond`, then `sudo systemctl restart lemond`.

**`lemond.service` with Docker:**

1. Remove Podman, which takes precedence: `sudo apt remove podman` or `sudo dnf remove podman`.
2. `sudo usermod -aG docker lemonade`, then `sudo systemctl restart lemond`.

Until setup is complete, the backend's state is `action_required`. `lemonade backends` lists every missing step and the commands that complete them, and the Backend Manager shows the same. The state updates as soon as a step takes effect.

## Install and Run

```bash
lemonade backends install llamacpp:nathanw
lemonade run Qwen3-4B-GGUF --llamacpp nathanw
```

Install pulls the image, so loading a model starts from the image already on disk. `lemonade backends uninstall llamacpp:nathanw` removes it.

## How Containers Run

Each loaded model gets its own container, named `lemonade-<recipe>-<backend>-<model>` and labeled `ai.lemonade`, so `podman ps --filter label=ai.lemonade` lists them. Under `lemond.service`, run Podman inside the service's private `/tmp`: `sudo systemd-run --quiet --wait --pty -p JoinsNamespaceOf=lemond.service -p PrivateTmp=yes -p User=lemonade -p WorkingDirectory=/ podman ps --filter label=ai.lemonade`. Plain `sudo -u lemonade podman` does not see the service's runtime state, and Podman 5 then loses track of its running containers. Every load logs its full `podman run` or `docker run` command. Each container:

- mounts only the model's files, read-only, under `/mnt/models`, with the shared SELinux label so other containers and programs can still read them
- opens only the GPU device nodes its backend needs
- drops every Linux capability its backend does not need, and runs with `no-new-privileges`
- joins its own `--internal` network, so only this machine reaches its API and the server inside stays off the internet

Unloading a model stops its container.
