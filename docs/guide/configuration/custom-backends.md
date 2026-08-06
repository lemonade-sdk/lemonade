# Custom Backends Configuration Guide

This guide explains how to define, configure, and register **Custom JSON Backend Descriptors** in Lemonade. Custom backends allow you to integrate external inference binaries (such as custom `llama-server` builds or `flm`) or containerized engines (such as Podman/Docker containers running speculative decoding servers) directly into Lemonade without modifying or recompiling the core C++ server.

---

## 1. Overview & Capabilities

Custom backends allow you to:
- **Run External Inference Engines**: Dispatch requests to standalone binaries or containerized LLM servers.
- **Isolate Environments & Hardware**: Pass per-process environment variables (`HIP_VISIBLE_DEVICES`, `CUDA_VISIBLE_DEVICES`, `GGML_VK_VISIBLE_DEVICES`).
- **Dynamic Argument Interpolation**: Substitute dynamic tokens like `{port}`, `{host}`, `{resolved_path}`, `{custom:opt:-default}`, and `{custom_args}` at load time.
- **Container Lifecycle Supervision**: Automatically clean up and stop Podman/Docker containers upon model unloading or server shutdown.
- **Fail-Fast Health Probing**: Validate process health via HTTP `/health` endpoints with configurable timeouts and instant process death detection.

---

## 2. Descriptor File Locations & Priority Hierarchy

Lemonade searches for custom backend descriptor `.json` files across standard user and system paths in priority order:

1. **User Configuration Directory (Highest Priority)**:
   - `$XDG_CONFIG_HOME/lemonade/backends/` or `~/.config/lemonade/backends/` (Linux / macOS)
   - `%APPDATA%\Lemonade\backends\` (Windows)
2. **User Cache Directory (Medium Priority)**:
   - `~/.cache/lemonade/backends/` (Linux / macOS)
   - `%USERPROFILE%\.cache\lemonade\backends\` (Windows)
3. **System Distribution Directories (Lowest Priority)**:
   - `/usr/share/lemonade-server/backends/`, `/usr/local/share/lemonade-server/backends/`, `/Library/Application Support/Lemonade/backends/`, `/etc/lemonade/backends/` (Linux / macOS)
   - `%ProgramData%\Lemonade\backends\` (Windows)

> [!IMPORTANT]
> **File Security Rules**:
> - **POSIX (Linux/macOS)**: Descriptors in user directories must be owned by the current user (`geteuid()`), and system descriptors must be owned by `root` (UID 0). Group-write and world-write permissions are strictly forbidden (`mode & 0022 == 0`). Ancestor directories must not be group/world writable unless they are owned by the user or have the sticky bit set (e.g. `/tmp` with `S_ISVTX`).
> - **Windows**: ACLs are verified via `GetNamedSecurityInfoW`. Write permissions assigned to `WinWorldSid` (Everyone) or `WinBuiltinUsersSid` (Users) are strictly rejected.

---

## 3. Creating a Custom Backend Descriptor

A backend descriptor is a JSON file named `<recipe-name>.json` placed in one of the backend directories.

### Example 1: Custom `llama-server` (Vulkan / ROCm)

Create `~/.config/lemonade/backends/llamacpp_vulkan_custom.json`:

```json
{
  "recipe": "llamacpp-vulkan-custom",
  "display_name": "llama.cpp (Vulkan Custom Binary)",
  "slot_policy": "standard",
  "capabilities": [
    "chat_completion",
    "completion"
  ],
  "endpoints": {
    "chat_completion": "/v1/chat/completions",
    "completion": "/v1/completions"
  },
  "health_probe": {
    "type": "http",
    "endpoint": "/health",
    "expected_status": 200,
    "timeout_seconds": 60,
    "poll_interval_ms": 100
  },
  "platforms": {
    "vulkan": {
      "command": "/home/user/.cache/lemonade/bin/llamacpp/vulkan/llama-server",
      "args": [
        "-m",
        "{resolved_path}",
        "--host",
        "{host}",
        "--port",
        "{port}",
        "-c",
        "{ctx_size}",
        "-t",
        "{custom:threads:-4}"
      ],
      "env": {
        "GGML_VK_VISIBLE_DEVICES": "{ggml_vk_visible_devices}"
      }
    }
  }
}
```

---

### Example 2: Containerized Backend (Podman ROCm Container)

Create `~/.config/lemonade/backends/dflash_rocm.json`:

```json
{
  "recipe": "dflash-rocm",
  "display_name": "DFlash Speculative Decoding Server (ROCm Podman)",
  "slot_policy": "standard",
  "capabilities": [
    "chat_completion",
    "completion"
  ],
  "health_probe": {
    "type": "http",
    "endpoint": "/health",
    "expected_status": 200,
    "timeout_seconds": 300,
    "poll_interval_ms": 500
  },
  "platforms": {
    "rocm": {
      "command": "podman",
      "args": [
        "run",
        "--rm",
        "--init",
        "-i",
        "--name",
        "lemonade-{recipe}-{port}",
        "--device",
        "/dev/kfd",
        "--device",
        "/dev/dri",
        "--group-add",
        "video",
        "--group-add",
        "render",
        "--security-opt",
        "seccomp=unconfined",
        "--security-opt",
        "label=disable",
        "-v",
        "{hf_cache}:/models:ro",
        "--network",
        "host",
        "--entrypoint",
        "/opt/lucebox-hub/server/build/dflash_server",
        "ghcr.io/luce-org/lucebox-hub:rocm-7.2",
        "/models/{checkpoint_relative:main}",
        "--target-device={target_device}",
        "--model-name={model_name}",
        "--host={host}",
        "--port={port}",
        "--max-ctx={ctx_size}",
        "--cache-type-k={cache_type_k}",
        "--cache-type-v={cache_type_v}",
        "--draft=/models/{checkpoint_relative:draft}",
        "{custom_args}"
      ],
      "stop_command": "podman",
      "stop_command_args": [
        "rm",
        "-f",
        "-t",
        "0",
        "lemonade-{recipe}-{port}"
      ],
      "env": {
        "DFLASH_DRAFT_SWA": "{custom:draft_swa:-2048}"
      }
    }
  }
}
```

> [!TIP]
> **Container Signal Handling**: Always pass `--init` to `podman run` or `docker run`. This injects a lightweight init process as PID 1 inside the container, ensuring signals like `SIGTERM` and `SIGKILL` propagate instantly to the child process when unloading.

---

## 4. Supported Token Placeholders

Lemonade dynamically interpolates placeholders in `args`, `env`, and `stop_command_args` at load time:

| Token Placeholder | Resolution Source | Description |
| :--- | :--- | :--- |
| `{port}` | Runtime | Assigned HTTP port for backend server process. |
| `{host}` | Runtime | Configured host address (e.g. `127.0.0.1`). |
| `{log_level}` | Runtime | Configured server log level (`info`, `debug`, etc.). |
| `{recipe}` | Descriptor JSON | Unique recipe identifier string. |
| `{model_name}` | Request Payload | Fully qualified model name requested by the client. |
| `{resolved_path}` | `ModelInfo` | Absolute file system path to the primary model checkpoint file. |
| `{model_dir}` | `ModelInfo` | Absolute parent directory path of the primary model checkpoint file. |
| `{exe_dir}` | Backend Execution | Absolute parent directory path of the backend command binary. |
| `{hf_cache}` | Environment / Cache | Hugging Face cache root directory (`~/.cache/huggingface/hub`). |
| `{model_relative_path}` | `ModelInfo` | Relative path from HF cache root to primary model checkpoint. |
| `{checkpoint:<name>}` | `user_models.json` | Absolute path to named secondary checkpoint (e.g. `draft`). |
| `{checkpoint_relative:<name>}` | `user_models.json` | Relative path to named secondary checkpoint from HF cache. |
| `{rocm_arch}` | Hardware Topology | Detected AMD ROCm GPU architecture string (e.g. `gfx1151`). |
| `{cuda_arch}` | Hardware Topology | Detected NVIDIA CUDA compute capability string (e.g. `sm_90`). |
| `{cuda_visible_devices}` | Hardware Topology | GPU index for CUDA allocation. |
| `{hip_visible_devices}` | Hardware Topology | GPU index for ROCm/HIP allocation. |
| `{rocr_visible_devices}` | Hardware Topology | GPU index for ROCR/HIP allocation. |
| `{ggml_vk_visible_devices}` | Hardware Topology | Device index for Vulkan allocation. |
| `{ze_affinity_mask}` | Hardware Topology | Device index for Level Zero / Intel GPU allocation. |
| `{env:VAR:-default}` | System Environment | Reads environment variable `VAR`. Uses `default` if not set. |
| `{custom:opt:-default}` | `RecipeOptions` | Reads custom option `opt` passed in model recipe options. Uses `default` if omitted. |
| `{custom_args}` | `RecipeOptions` | Expands extra command-line flags. Accepts JSON arrays or shell-quoted strings. |

---

## 5. Registering a Custom Model

Once your backend descriptor is saved, reference its `recipe` in `user_models.json`:

```json
{
  "gemma3-270m-vulkan-custom": {
    "checkpoint": "unsloth/gemma-3-270m-it-GGUF:gemma-3-270m-it-UD-IQ2_M.gguf",
    "recipe": "llamacpp-vulkan-custom",
    "recipe_options": {
      "ctx_size": 4096,
      "threads": 8
    }
  }
}
```

Now dispatch requests using standard OpenAI REST endpoints:

```bash
curl -X POST http://localhost:11434/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gemma3-270m-vulkan-custom",
    "messages": [{"role": "user", "content": "Hello!"}]
  }'
```

---

## 6. Troubleshooting & Logs

To inspect backend startup output or debug container launching:
1. Stream server logs via `/v1/logs/stream` or check `lemond.log`.
2. Inspect active processes using `ps aux | grep <command>`.
3. Check container status using `podman ps -a` or `docker ps -a`.
