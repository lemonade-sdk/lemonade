# Lemonade Server Configuration

## Overview

Lemonade Server starts automatically with the OS after installation. This page covers customizing server behavior via environment variables and configuration files.

## Configuration Methods

### Linux: Configuration Files

The systemd service reads configuration files in this order:

1. `/etc/lemonade/lemonade.conf` — base settings
2. `/etc/lemonade/conf.d/*.conf` — optional overrides, loaded in alphabetical order

For most users, editing the base file is sufficient:

```bash
sudo nano /etc/lemonade/lemonade.conf
```

For advanced setups, you can add drop-in files under `conf.d/` to keep local overrides separate from the package-provided base config (e.g., `50-local.conf` for general overrides, `zz-secrets.conf` for `LEMONADE_API_KEY`).

After making changes, restart the service:

```bash
sudo systemctl restart lemonade-server
```

### Windows: User Environment Variables

Set environment variables via **System Properties > Environment Variables**, or from the command line:

```cmd
setx LEMONADE_PORT 8080
setx LEMONADE_LOG_LEVEL debug
```

After making changes, quit from the tray icon, then relaunch from the Start Menu.

### macOS: Environment Variables

Set environment variables in your shell profile or launchd plist. After making changes, restart from the tray icon or via `launchctl`.

## Environment Variables

These settings are recognized by Lemonade Server regardless of launch method:

| Environment Variable               | Description                                                                                                                                             |
|------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| `LEMONADE_HOST`                    | Host address for where to listen for connections                                                                                                        |
| `LEMONADE_PORT`                    | Port number to run the server on                                                                                                                        |
| `LEMONADE_LOG_LEVEL`               | Logging level                                                                                                                                           |
| `LEMONADE_LLAMACPP`                | Default LlamaCpp backend (`vulkan`, `rocm`, or `cpu`)                                                                                                   |
| `LEMONADE_WHISPERCPP`              | Default WhisperCpp backend: `npu` or `cpu` on Windows; `cpu` or `vulkan` on Linux                                                                       |
| `LEMONADE_CTX_SIZE`                | Default context size for models                                                                                                                         |
| `LEMONADE_LLAMACPP_ARGS`           | Custom arguments to pass to llama-server                                                                                                                |
| `LEMONADE_WHISPERCPP_ARGS`         | Custom arguments to pass to whisper-server (for example `--convert`)                                                                                    |
| `LEMONADE_FLM_ARGS`                | Custom arguments to pass to FLM server                                                                                                                  |
| `LEMONADE_EXTRA_MODELS_DIR`        | Secondary directory to scan for GGUF model files                                                                                                        |
| `LEMONADE_MAX_LOADED_MODELS`       | Maximum number of models to keep loaded per type slot (LLMs, audio, image, etc.). Use `-1` for unlimited, or a positive integer. Default: `1`           |
| `LEMONADE_DISABLE_MODEL_FILTERING` | Set to `1` to disable hardware-based model filtering (e.g., RAM amount, NPU availability) and show all models regardless of system capabilities         |
| `LEMONADE_ENABLE_DGPU_GTT`         | Set to `1` to include GTT for hardware-based model filtering |
| `LEMONADE_GLOBAL_TIMEOUT`          | Global default timeout for HTTP requests, inference, and readiness checks in seconds |

## Custom Backend Binaries

You can provide your own `llama-server`, `whisper-server`, or `ryzenai-server` binary by setting the full path via the following environment variables:

| Environment Variable | Description |
|---------------------|-------------|
| `LEMONADE_LLAMACPP_ROCM_BIN` | Path to custom `llama-server` binary for ROCm backend |
| `LEMONADE_LLAMACPP_VULKAN_BIN` | Path to custom `llama-server` binary for Vulkan backend |
| `LEMONADE_LLAMACPP_CPU_BIN` | Path to custom `llama-server` binary for CPU backend |
| `LEMONADE_WHISPERCPP_CPU_BIN` | Path to custom `whisper-server` binary for CPU backend |
| `LEMONADE_WHISPERCPP_NPU_BIN` | Path to custom `whisper-server` binary for NPU backend |
| `LEMONADE_RYZENAI_SERVER_BIN` | Path to custom `ryzenai-server` binary for NPU/Hybrid models |

**Note:** These environment variables do not override the `--llamacpp` option. They allow you to specify an alternative binary for specific backends while still using the standard backend selection mechanism.

**Examples:**

On Windows:

```cmd
setx LEMONADE_LLAMACPP_VULKAN_BIN "C:\path\to\my\llama-server.exe"
```

On Linux (in `/etc/lemonade/lemonade.conf`):

```bash
LEMONADE_LLAMACPP_VULKAN_BIN=/path/to/my/llama-server
```

## API Key and Security

If you expose your server over a network you can use the `LEMONADE_API_KEY` environment variable to set an API key (use a random long string) that will be required to execute any request. The API key will be expected as HTTP Bearer authentication, which is compatible with the OpenAI API.

**IMPORTANT**: If you need to access Lemonade Server over the internet, do not expose it directly! You will also need to setup an HTTPS reverse proxy (such as nginx) and expose that instead, otherwise all communication will be in plaintext!

## Timeout Configuration

Lemonade uses a unified timeout strategy controlled by the `LEMONADE_GLOBAL_TIMEOUT` environment variable. This value ensures stability across different operations:

| Timeout Name | Default | Description |
|--------------|---------|-------------|
| **Global HTTP Timeout** | 300s | Sets the base timeout for all `curl` operations, including model downloads and management tasks. |
| **Inference Timeout** | 300s | Applied specifically to inference requests (chat, completion) to backends. For very long generations, increasing the timeout may be necessary. |
| **Readiness Timeout** | 300s* | Maximum time the router waits for a backend server to become healthy after starting it. *Note: If not explicitly set, backends may use up to 600s for initial setup. |

## Remote Server Connection

To make Lemonade Server accessible from other machines on your network, set the host to `0.0.0.0`:

**Linux** (in `/etc/lemonade/lemonade.conf`):

```bash
LEMONADE_HOST=0.0.0.0
```

Then restart:

```bash
sudo systemctl restart lemonade-server
```

**Windows:**

```cmd
setx LEMONADE_HOST 0.0.0.0
```

Then quit from the tray icon, then relaunch from the Start Menu.

> **Note:** Using `LEMONADE_HOST=0.0.0.0` allows connections from other machines on the network. Only do this on trusted networks. Set `LEMONADE_API_KEY` (see above) to manage access on your network.

For developers running `lemonade-router` directly: `lemonade-router --host 0.0.0.0`

## Restarting the Server

After changing configuration, you need to restart the server for changes to take effect:

- **Linux:** `sudo systemctl restart lemonade-server`
- **Windows:** Quit from the tray icon, then relaunch from the Start Menu
- **macOS:** Restart from the tray icon or via `launchctl`

## Next Steps

The [Integration Guide](./server_integration.md) provides more information about how to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->
