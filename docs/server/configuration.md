# Lemonade Server Configuration

## Overview

Lemonade Server starts automatically with the OS after installation. Configuration is managed through a single `config.json` file stored in the lemonade home directory.

## config.json

All settings are in `config.json`, located in the lemonade home directory:

- **Linux (systemd):** `/var/lib/lemonade/config.json`
- **Windows:** `%USERPROFILE%\.cache\lemonade\config.json`
- **macOS:** `~/.cache/lemonade/config.json`

If `config.json` doesn't exist, it's created automatically with default values on first run.

### Example config.json

```json
{
  "config_version": 1,
  "port": 8000,
  "host": "localhost",
  "log_level": "info",
  "global_timeout": 300,
  "max_loaded_models": 1,
  "no_broadcast": false,
  "extra_models_dir": "",
  "models_dir": "auto",
  "ctx_size": 4096,
  "offline": false,
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
    "linux_beta": false
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
| `port` | int | 8000 | Port number for the HTTP server |
| `host` | string | "localhost" | Address to bind for connections |
| `log_level` | string | "info" | Logging level (trace, debug, info, warning, error, fatal, none) |
| `global_timeout` | int | 300 | Timeout in seconds for HTTP, inference, and readiness checks |
| `max_loaded_models` | int | 1 | Max models per type slot. Use -1 for unlimited |
| `no_broadcast` | bool | false | Disable UDP broadcasting for server discovery |
| `extra_models_dir` | string | "" | Secondary directory to scan for GGUF model files |
| `models_dir` | string | "auto" | Directory for cached model files. "auto" follows HF_HUB_CACHE / HF_HOME / platform default |
| `ctx_size` | int | 4096 | Default context size for LLM models |
| `offline` | bool | false | Skip model downloads |
| `disable_model_filtering` | bool | false | Show all models regardless of hardware capabilities |
| `enable_dgpu_gtt` | bool | false | Include GTT for hardware-based model filtering |

### Backend Configuration

Backend-specific settings are nested under their backend name:

**llamacpp** — LLM inference via llama.cpp:
| Key | Default | Description |
|-----|---------|-------------|
| `backend` | "auto" | Backend to use: "auto", "vulkan", "rocm", or "cpu" |
| `args` | "" | Custom arguments to pass to llama-server |
| `prefer_system` | false | Prefer system-installed llama.cpp over bundled |
| `*_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**whispercpp** — Audio transcription:
| Key | Default | Description |
|-----|---------|-------------|
| `backend` | "auto" | Backend to use: "auto", "cpu", "npu" (Windows), "vulkan" (Linux) |
| `args` | "" | Custom arguments to pass to whisper-server |
| `*_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**sdcpp** — Image generation:
| Key | Default | Description |
|-----|---------|-------------|
| `backend` | "auto" | Backend to use: "auto", "cpu", "rocm", "vulkan" |
| `steps` | 20 | Number of inference steps |
| `cfg_scale` | 7.0 | Classifier-free guidance scale |
| `width` | 512 | Image width in pixels |
| `height` | 512 | Image height in pixels |
| `*_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**flm** — FastFlowLM NPU inference:
| Key | Default | Description |
|-----|---------|-------------|
| `args` | "" | Custom arguments to pass to flm serve |
| `linux_beta` | false | Enable Linux beta support |

**ryzenai** — RyzenAI NPU inference:
| Key | Default | Description |
|-----|---------|-------------|
| `server_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

**kokoro** — Text-to-speech:
| Key | Default | Description |
|-----|---------|-------------|
| `cpu_bin` | "builtin" | Path to custom binary, or "builtin" for bundled |

## Editing Configuration

### Option 1: Edit config.json directly

```bash
# Linux
sudo nano /var/lib/lemonade/config.json
sudo systemctl restart lemonade-server

# Windows — edit in your preferred text editor:
# %USERPROFILE%\.cache\lemonade\config.json
# Then quit and relaunch from the Start Menu
```

### Option 2: Runtime API

Changes can be applied at runtime via the `/internal/set` endpoint (loopback only):

```bash
# Change a top-level setting
curl -X POST localhost:8000/internal/set \
  -H "Content-Type: application/json" \
  -d '{"log_level": "debug"}'

# Change a backend setting
curl -X POST localhost:8000/internal/set \
  -H "Content-Type: application/json" \
  -d '{"llamacpp": {"backend": "vulkan"}}'

# Read current config
curl localhost:8000/internal/config
```

Changes made via `/internal/set` are persisted to config.json immediately.

### Option 3: CLI overrides

`lemond` accepts `--port` and `--host` as CLI arguments. These override config.json and are persisted:

```bash
lemond --port 9000 --host 0.0.0.0
```

## lemond CLI

```
lemond [home_dir] [--port PORT] [--host HOST]
```

- **home_dir** — Path to the lemonade home directory containing config.json and model data. Optional; defaults to platform-specific location.
- **--port** — Port to serve on (overrides config.json, persisted)
- **--host** — Address to bind (overrides config.json, persisted)

## API Key and Security

The `LEMONADE_API_KEY` environment variable sets an API key for authentication. On Linux with systemd, set it in `/etc/lemonade/conf.d/zz-secrets.conf`:

```bash
LEMONADE_API_KEY=your-secret-key
```

## Remote Server Connection

To make Lemonade Server accessible from other machines on your network, set the host to `0.0.0.0`:

```bash
# Via config.json
{"host": "0.0.0.0"}

# Or via CLI
lemond --host 0.0.0.0

# Or via runtime API
curl -X POST localhost:8000/internal/set -d '{"host":"0.0.0.0"}'
```

> **Note:** Using `host: "0.0.0.0"` allows connections from any machine on the network. Only do this on trusted networks. Set `LEMONADE_API_KEY` to manage access.

## Migration from Environment Variables

If upgrading from a version that used environment variables, you must migrate manually using `lemonade config set`. For example, if you previously had `LEMONADE_PORT=9000` and `LEMONADE_LLAMACPP=rocm`, run:

```bash
lemonade config set port=9000 llamacpp.backend=rocm
```

Nested backend settings use dot notation: `section.key=value` (e.g., `sdcpp.steps=30`, `whispercpp.backend=cpu`). Top-level settings use their JSON key name directly (e.g., `port=8000`, `log_level=debug`).

Run `lemonade config` to view all current settings and their `config set` key names.

## Next Steps

The [Integration Guide](./server_integration.md) provides more information about how to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->
