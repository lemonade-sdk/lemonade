# Lemonade Server Configuration

## Overview

Lemonade Server starts automatically with the OS after installation. Configuration is managed through a single `config.json` file stored in the lemonade cache directory.

## config.json

If you used an installer from the Lemonade release your `config.json` will be at these locations depending on your OS:

- **Linux (systemd):** `/var/lib/lemonade/.cache/lemonade/config.json`
- **Windows:** `%USERPROFILE%\.cache\lemonade\config.json`
- **macOS:** `/Library/Application Support/lemonade/.cache/config.json`

If you are using a standalone `lemond` exectable, the default location is `~/.cache/lemonade/config.json`.

> Note: If `config.json` doesn't exist, it's created automatically with default values on first run.

### Example config.json

```json
{
  "config_version": 1,
  "port": 13305,
  "host": "localhost",
  "external_url": "",
  "websocket_port": "auto",
  "log_level": "info",
  "global_timeout": 300,
  "max_loaded_models": 1,
  "no_broadcast": false,
  "extra_models_dir": "",
  "models_dir": "auto",
  "ctx_size": 4096,
  "offline": false,
  "no_fetch_executables": false,
  "disable_model_filtering": false,
  "enable_dgpu_gtt": false,
  "llamacpp": {
    "backend": "auto",
    "args": "",
    "prefer_system": false,
    "rocm_bin": "builtin",
    "vulkan_bin": "builtin",
    "cpu_bin": "builtin"
  },
  "whispercpp": {
    "backend": "auto",
    "args": "",
    "cpu_bin": "builtin",
    "npu_bin": "builtin"
  },
  "sdcpp": {
    "backend": "auto",
    "args": "",
    "steps": 20,
    "cfg_scale": 7.0,
    "width": 512,
    "height": 512,
    "cpu_bin": "builtin",
    "rocm_bin": "builtin",
    "vulkan_bin": "builtin"
  },
  "flm": {
    "args": "",
  },
  "ryzenai": {
    "server_bin": "builtin"
  },
  "kokoro": {
    "cpu_bin": "builtin"
  }
}
```

### Settings Reference

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `port` | int | 13305 | Port number for the HTTP server |
| `host` | string | "localhost" | Address to bind for connections |
| `external_url` | string | "" | Public browser-facing base URL for the web app when served behind a reverse proxy. Must be an `http://` or `https://` URL. May include a path prefix such as `https://example.com/lemonade` |
| `websocket_port` | int or `"auto"` | `"auto"` | Port for the standalone WebSocket server used by `/realtime` and `/logs/stream`. Use a fixed value for reverse-proxy deployments |
| `log_level` | string | "info" | Logging level (trace, debug, info, warning, error, fatal, none) |
| `global_timeout` | int | 300 | Timeout in seconds for HTTP, inference, and readiness checks |
| `max_loaded_models` | int | 1 | Max models per type slot. Use -1 for unlimited |
| `no_broadcast` | bool | false | Disable UDP broadcasting for server discovery |
| `extra_models_dir` | string | "" | Secondary directory to scan for GGUF model files |
| `models_dir` | string | "auto" | Directory for cached model files. "auto" follows HF_HUB_CACHE / HF_HOME / platform default |
| `ctx_size` | int | 4096 | Default context size for LLM models |
| `offline` | bool | false | Skip model downloads |
| `no_fetch_executables` | bool | false | Prevent downloading backend executable artifacts; backends must already be installed or use the system backend |
| `disable_model_filtering` | bool | false | Show all models regardless of hardware capabilities |
| `enable_dgpu_gtt` | bool | false | Include GTT for hardware-based model filtering |

### Backend Configuration

Backend-specific settings are nested under their backend name:

**llamacpp** — LLM inference via llama.cpp:
| Key | Default | Description |
|-----|---------|-------------|
| `backend` | "auto" | Backend to use: "auto" means "choose for me" |
| `args` | "" | Custom arguments to pass to llama-server |
| `prefer_system` | false | Prefer system-installed llama.cpp over bundled |
| `*_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**whispercpp** — Audio transcription:
| Key | Default | Description |
|-----|---------|-------------|
| `backend` | "auto" | Backend to use: "auto" means "choose for me" |
| `args` | "" | Custom arguments to pass to whisper-server |
| `*_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**sdcpp** — Image generation:
| Key | Default | Description |
|-----|---------|-------------|
| `backend` | "auto" | Backend to use: "auto" means "choose for me" |
| `args` | "" | Custom arguments to pass to `sd-server` |
| `steps` | 20 | Number of inference steps |
| `cfg_scale` | 7.0 | Classifier-free guidance scale |
| `width` | 512 | Image width in pixels |
| `height` | 512 | Image height in pixels |
| `*_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**flm** — FastFlowLM NPU inference:
| Key | Default | Description |
|-----|---------|-------------|
| `args` | "" | Custom arguments to pass to flm serve |

**ryzenai** — RyzenAI NPU inference:
| Key | Default | Description |
|-----|---------|-------------|
| `server_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**kokoro** — Text-to-speech:
| Key | Default | Description |
|-----|---------|-------------|
| `cpu_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

## Editing Configuration

### lemonade config (recommended)

Use the `lemonade config` CLI to view and modify settings while the server is running. Changes are applied immediately and persisted to config.json.

```bash
# View all current settings
lemonade config

# Set one or more values
lemonade config set key=value [key=value ...]
```

Top-level settings use their JSON key name directly. Nested backend settings use dot notation (`section.key=value`):

```bash
# Change the server port and log level
lemonade config set port=9000 log_level=debug

# Configure a public URL for the web app behind a reverse proxy
lemonade config set external_url=https://example.com/lemonade

# Pin the WebSocket port for reverse-proxy routing
lemonade config set websocket_port=9000

# Change a backend setting
lemonade config set llamacpp.backend=rocm

# Set multiple values at once
lemonade config set port=9000 llamacpp.backend=rocm sdcpp.steps=30
```

### lemond CLI arguments (fallback)

If the server cannot start (e.g., invalid port in config.json), `lemond` accepts `--port` and `--host` as CLI arguments to override config.json. These overrides are persisted so the server can start normally next time:

```bash
lemond --port 9000 --host 0.0.0.0
```

### Edit config.json manually (last resort)

If the server won't start and CLI arguments aren't sufficient, you can edit config.json directly. Restart the server after making changes:

```bash
# Linux
sudo nano /var/lib/lemonade/.cache/lemonade/config.json
sudo systemctl restart lemonade-server

# Windows — edit with your preferred text editor:
# %USERPROFILE%\.cache\lemonade\config.json
# Then quit and relaunch from the Start Menu
```

## lemond CLI

```
lemond [cache_dir] [--port PORT] [--host HOST]
```

- **cache_dir** — Path to the lemonade cache directory containing config.json and model data. Optional; defaults to platform-specific location.
- **--port** — Port to serve on (overrides config.json, persisted). Use as a fallback if the server cannot start.
- **--host** — Address to bind (overrides config.json, persisted). Use as a fallback if the server cannot start.

## API Key and Security

### Regular API Key

The `LEMONADE_API_KEY` environment variable sets an API key for authentication on regular API endpoints (`/api/*`, `/v0/*`, `/v1/*`). On Linux with systemd, set it in the service environment (e.g., via a systemd override or drop-in file). On Windows, set it as a system environment variable.

### Admin API Key

The `LEMONADE_ADMIN_API_KEY` environment variable provides elevated access to both regular API endpoints and internal endpoints (`/internal/*`). When set, it takes precedence over `LEMONADE_API_KEY` for client authentication.

**Authentication Hierarchy:**

| Scenario | `LEMONADE_API_KEY` | `LEMONADE_ADMIN_API_KEY` | Internal Endpoints | Regular API Endpoints |
|----------|-------------------|--------------------------|-------------------|----------------------|
| No keys set | (not set) | (not set) | No auth required | No auth required |
| Only API key | "secret" | (not set) | Requires key | Requires key |
| Only admin key | (not set) | "admin" | Requires admin key | No auth required |
| Both keys different | "regular" | "admin" | Requires admin key | Either key accepted |

**Client Behavior:** Clients (CLI, tray app) automatically prefer `LEMONADE_ADMIN_API_KEY` if set, otherwise fall back to `LEMONADE_API_KEY`.

## Remote Server Connection

To make Lemonade Server accessible from other machines on your network, set the host to `0.0.0.0`:

```bash
lemonade config set host=0.0.0.0
```

> **Note:** Using `host: "0.0.0.0"` allows connections from any machine on the network. Only do this on trusted networks. Set `LEMONADE_API_KEY` or `LEMONADE_ADMIN_API_KEY` to manage access.

## Reverse Proxy Deployments

When the Lemonade web app is published through a reverse proxy, configure the server with a public URL and a fixed WebSocket port:

```bash
lemonade config set external_url=https://example.com/lemonade websocket_port=9000
```

`external_url` is used by the browser-facing web app to derive:

- REST API calls under `<external_url>/api/v1/...`
- Realtime transcription at `<external_url>/realtime`
- Log streaming at `<external_url>/logs/stream`

Your reverse proxy should route:

- `<external_url>/api/*` and web app assets to Lemonade's HTTP server (`port`)
- `<external_url>/realtime` to Lemonade's WebSocket server (`websocket_port`) with WebSocket upgrade enabled
- `<external_url>/logs/stream` to Lemonade's WebSocket server (`websocket_port`) with WebSocket upgrade enabled

If `websocket_port` is left at `"auto"`, Lemonade will still work for local/direct connections, but reverse proxies cannot reliably target a stable upstream WebSocket port.

## Next Steps

The [Integration Guide](./server_integration.md) provides more information about how to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->
