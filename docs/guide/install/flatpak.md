# Install Lemonade on Linux (Flatpak)

Lemonade is available as a sandboxed Flatpak application supporting both **Stable** releases and **Nightly** development builds with full GPU and NPU acceleration.

---

## 1. Quick Install

### Add the Lemonade Repository
```bash
flatpak remote-add --if-not-exists lemonade https://lemonade-server.ai/flatpak/lemonade.flatpakrepo
```

### Install Stable Release
```bash
flatpak install lemonade ai.lemonadeserver.app
```

### Install Nightly Build
To try the latest features from the development branch:
```bash
flatpak install lemonade ai.lemonadeserver.app//nightly
```

---

## 2. Running Lemonade

Launch the desktop UI and background server:
```bash
flatpak run ai.lemonadeserver.app
```

To run the CLI client inside the Flatpak environment:
```bash
flatpak run --command=lemonade ai.lemonadeserver.app list
flatpak run --command=lemonade ai.lemonadeserver.app run llama-3.2-1b
```

---

## 3. Hardware Acceleration & Permissions

The Lemonade Flatpak is configured with hardware acceleration and shared host cache access:

- **GPU & NPU Acceleration**: GPU compute is enabled via `--device=dri`, and NPU/GPU architecture discovery is supported via read-only sysfs topology access (`/sys/class/kfd:ro`, `/sys/class/accel:ro`).
- **Shared Model & Config Cache**: Reads and writes to `~/.cache/huggingface`, `~/.cache/lemonade`, and `~/.cache/modelscope` on the host, preventing duplicate model downloads.
- **Local Model Import**: Read-only access to `~/Downloads` and `~/Documents` allows importing local GGUF/Safetensors checkpoints via CLI or GUI.

---

## 4. Upgrades & Channel Switching

To update Lemonade:
```bash
flatpak update ai.lemonadeserver.app
```

To switch between Stable and Nightly channels:
```bash
# Switch to nightly
flatpak install --reinstall lemonade ai.lemonadeserver.app//nightly

# Switch back to stable
flatpak install --reinstall lemonade ai.lemonadeserver.app//stable
```
