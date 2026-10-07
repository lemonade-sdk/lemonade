# Internal API

Internal endpoints let the clients bundled with the server, such as the desktop app, the tray and the `lemonade` CLI, control and configure it. They are served only under `/internal/`, and `LEMONADE_ADMIN_API_KEY` secures them when it is set, which separates control privileges from inference. When only `LEMONADE_API_KEY` is set, it secures them too.

<!-- BEGIN GENERATED: internal.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/internal/shutdown`](#post-internalshutdown) | Unload every model and stop lemond |
| `POST` | [`/internal/telemetry/flush`](#post-internaltelemetryflush) | Force-flush all queued telemetry trace spans |
| `POST` | [`/internal/pin`](#post-internalpin) | Pin or unpin a loaded model |
| `POST` | [`/internal/set`](#post-internalset) | Change config settings at runtime |
| `GET` | [`/internal/config`](#get-internalconfig) | Read the runtime config |
| `GET` | [`/internal/config/defaults`](#get-internalconfigdefaults) | Read the factory default config |
| `POST` | [`/internal/cleanup-cache`](#post-internalcleanup-cache) | Find or remove orphaned model files |
| `POST` | [`/internal/simulate-vram-pressure`](#post-internalsimulate-vram-pressure) | Run the eviction engine at a simulated VRAM usage |
| `POST` | [`/internal/models/sync`](#post-internalmodelssync) | Download updates for downloaded models |
| `GET` | [`/internal/models/sync/status`](#get-internalmodelssyncstatus) | Read the progress of a model sync |
| `GET` | [`/internal/aliases`](#get-internalaliases) | List all active model aliases |
| `POST` | [`/internal/aliases`](#post-internalaliases) | Create or update a model alias |
| `DELETE` | [`/internal/aliases/{alias}`](#delete-internalaliasesalias) | Remove a model alias |
| `GET` | [`/internal/routes`](#get-internalroutes) | List every route's specification |
<!-- END GENERATED: internal.summary -->

<!-- BEGIN GENERATED: internal.shutdown -->
## `POST /internal/shutdown`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Unloads every model, answers, and then stops lemond.

Models unload before the response is sent, so backend processes such as `llama-server` have exited by the time the caller reads it. lemond cancels its downloads and exits about 100 ms later.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/shutdown" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/internal/shutdown
    ```

=== "Response"

    `200`

    ```json
    {"status": "shutting down"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "shutting down"}}}
    ```
<!-- END GENERATED: internal.shutdown -->

<!-- BEGIN GENERATED: internal.telemetry_flush -->
## `POST /internal/telemetry/flush`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Sends every queued trace span to the configured OTLP collector now, and answers once they are serialized and sent.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/telemetry/flush" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/internal/telemetry/flush
    ```

=== "Response"

    `200`

    ```json
    {"status": "flushed"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "flushed"}}}
    ```
<!-- END GENERATED: internal.telemetry_flush -->

<!-- BEGIN GENERATED: internal.pin -->
## `POST /internal/pin`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Pins or unpins a loaded model without reloading it. Least-recently-used eviction skips pinned models.

A load that needs a slot held only by pinned models fails with `409` and a `slots_pinned_error` code; see [Model Pinning](../guide/configuration/multi-model.md#model-pinning).

A `400` answers invalid JSON, a missing model name, a missing or non-boolean `pinned`, or a model that is not loaded.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | Loaded model to pin or unpin. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `pinned` | Yes | `true` pins the model; `false` unpins it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/pin" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF", "pinned": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/pin \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF", "pinned": true}'
    ```

=== "Response"

    `200`

    ```json
    {"model_name": "Qwen3-0.6B-GGUF", "pinned": true, "status": "success"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "model_name", "pinned"],
      "properties": {
        "model_name": {"description": "The model name as sent.", "type": "string"},
        "pinned": {"description": "The model's pinned state now.", "type": "boolean"},
        "status": {"const": "success"}
      }
    }
    ```
<!-- END GENERATED: internal.pin -->

<!-- BEGIN GENERATED: internal.set -->
## `POST /internal/set`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Changes one or more `config.json` settings without restarting lemond, and saves them to `config.json`.

Every key is validated before any is applied, so one invalid value rejects the whole request with `400` and changes nothing.

Settings with a runtime effect apply immediately: a `port` or `host` change rebinds the listeners, `log_level` reconfigures logging, and a backend section's `*_bin` change reinstalls that backend and reloads its models. The rest apply to the next model load or eviction decision. See the [Settings Reference](../guide/configuration/README.md#settings-reference) for every key.

`config.json` keeps only values that differ from the defaults, so setting a key to its default removes it from the file. The `lemonade config set` command uses this endpoint.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `acestep` | No | `acestep` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allowed_origins` | No | Default `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_check_model_updates` | No | Default `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_evict` | No | Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_evict_threshold_pct` | No | Default `0.9`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_update_models` | No | Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `broadcast` | No | Default `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `config_version` | No | Default `2`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Default `-1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `default_model_source` | No | Default `"huggingface"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `disable_model_filtering` | No | Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `download_rate_limit` | No | Default `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ds4` | No | `ds4` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `enable_dgpu_gtt` | No | Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `extra_models_dir` | No | Default `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `flm` | No | `flm` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `global_timeout` | No | Default `600`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `host` | No | Default `"localhost"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `hrx` | No | `hrx` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `inhibit_suspend` | No | Default `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `kokoro` | No | `kokoro` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp` | No | `llamacpp` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_file` | No | Default `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_level` | No | Default `"info"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_max_file_size_mb` | No | Default `10`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_max_files` | No | Default `5`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_loaded_models` | No | Default `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `models_dir` | No | Default `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `moonshine` | No | `moonshine` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `no_fetch_executables` | No | Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `offline` | No | Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `onnxruntime` | No | `onnxruntime` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `openmoss` | No | `openmoss` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `port` | No | Default `13305`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `rocm_channel` | No | Default `"stable"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `rocm_install_method` | No | Default `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ryzenai` | No | `ryzenai` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `sdcpp` | No | `sdcpp` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `telemetry` | No | `telemetry` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `thenoise` | No | `thenoise` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `thinksound` | No | `thinksound` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `trellis` | No | `trellis` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vllm` | No | `vllm` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `websocket_port` | No | Default `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp` | No | `whispercpp` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/set" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"log_level": "info"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/set \
      -H "Content-Type: application/json" \
      -d '{"log_level": "info"}'
    ```

=== "Response"

    `200`

    ```json
    {"status": "success", "updated": {"log_level": "info"}}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "updated"],
      "properties": {
        "message": {
          "description": "Present when a legacy key was translated to its current form.",
          "type": "string"
        },
        "status": {"const": "success"},
        "updated": {
          "description": "The keys the request set, with their new values.",
          "type": "object"
        }
      }
    }
    ```
<!-- END GENERATED: internal.set -->

<!-- BEGIN GENERATED: internal.config -->
## `GET /internal/config`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the full runtime configuration: every `config.json` key, server-level and per-backend, with its current value.

The `lemonade config` command reads this endpoint. See the [Settings Reference](../guide/configuration/README.md#settings-reference) for each key.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/config"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/config
    ```

=== "Response"

    `200`

    ```json
    {
      "_generated": "GENERATED by docs/tools/gen_backend_boilerplate.py -- do not hand-edit per-recipe sections (they come from each backend's descriptor config_defaults()). Global keys are hand-maintained in this file. Regenerate and verify with that script; CI --check fails on drift.",
      "acestep": {
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "allowed_origins": "",
      "auto_check_model_updates": true,
      "auto_evict": false,
      "auto_evict_threshold_pct": 0.9,
      "auto_update_models": false,
      "broadcast": true,
      "cloud_providers": [],
      "config_version": 2,
      "ctx_size": -1,
      "default_model_source": "huggingface",
      "disable_model_filtering": false,
      "download_rate_limit": "",
      "ds4": {"args": ""},
      "enable_dgpu_gtt": false,
      "extra_models_dir": "",
      "flm": {"args": "", "prefer_system": false},
      "global_timeout": 600,
      "host": "localhost",
      "hrx": {"args": "", "hrx_bin": "builtin"},
      "inhibit_suspend": true,
      "kokoro": {"cpu_bin": "builtin"},
      "llamacpp": {
        "args": "",
        "backend": "auto",
        "cpu_args": "",
        "cpu_bin": "builtin",
        "cuda_bin": "builtin",
        "prefer_system": true,
        "rocm_args": "",
        "rocm_bin": "builtin",
        "vulkan_args": "",
        "vulkan_bin": "builtin"
      },
      "log_file": "auto",
      "log_level": "info",
      "log_max_file_size_mb": 10,
      "log_max_files": 5,
      "max_loaded_models": 1,
      "models_dir": "auto",
      "moonshine": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
      "no_fetch_executables": false,
      "offline": false,
      "onnxruntime": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
      "openmoss": {
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "port": 13305,
      "rocm_channel": "stable",
      "rocm_install_method": "auto",
      "ryzenai": {"server_bin": "builtin"},
      "sdcpp": {
        "args": "",
        "backend": "auto",
        "cfg_scale": 7.0,
        "cpu_args": "",
        "cpu_bin": "builtin",
        "cuda_args": "",
        "cuda_bin": "builtin",
        "height": 512,
        "rocm_args": "",
        "rocm_bin": "builtin",
        "steps": 20,
        "vulkan_args": "",
        "vulkan_bin": "builtin",
        "width": 512
      },
      "telemetry": {
        "enabled": false,
        "hide_inputs": false,
        "hide_outputs": false,
        "hide_thinking": false,
        "max_queue_capacity": 1000,
        "otlp": {
          "batch_timeout_s": 1.0,
          "endpoint": "http://localhost:4318/v1/traces",
          "headers": {},
          "max_retries": 0,
          "protocol": "http/protobuf",
          "retry_backoff_base_s": 5.0,
          "semantics": ["openinference", "otel_genai"],
          "send_batch_size": 100
        },
        "session": {"headers": {"client": [], "id": []}},
        "trust_incoming_trace_context": false
      },
      "thenoise": {"backend": "auto", "lora_dir": "", "rocm_bin": "builtin", "upscaler_dir": ""},
      "thinksound": {
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "trellis": {
        "args": "",
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "vllm": {"args": "", "backend": "auto"},
      "websocket_port": "auto",
      "whispercpp": {
        "args": "",
        "backend": "auto",
        "cpu_args": "",
        "cpu_bin": "builtin",
        "npu_args": "",
        "npu_bin": "builtin"
      }
    }
    ```

=== "Schema"

    ```json
    {
      "description": "Every config.json key with its current value.",
      "type": "object",
      "required": ["port", "host", "log_level"],
      "properties": {
        "host": {"type": "string"},
        "log_level": {"type": "string"},
        "port": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: internal.config -->

<!-- BEGIN GENERATED: internal.config_defaults -->
## `GET /internal/config/defaults`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the default configuration built into this release, independent of this instance's `config.json` and of any deployment override.

The per-backend sections come from the backend descriptors, so this is the authoritative list of factory defaults. `docs/tools/gen_backend_boilerplate.py` reads it to regenerate `src/cpp/resources/defaults.json`.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/config/defaults"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/config/defaults
    ```

=== "Response"

    `200`

    ```json
    {
      "_generated": "GENERATED by docs/tools/gen_backend_boilerplate.py -- do not hand-edit per-recipe sections (they come from each backend's descriptor config_defaults()). Global keys are hand-maintained in this file. Regenerate and verify with that script; CI --check fails on drift.",
      "acestep": {
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "allowed_origins": "",
      "auto_check_model_updates": true,
      "auto_evict": false,
      "auto_evict_threshold_pct": 0.9,
      "auto_update_models": false,
      "broadcast": true,
      "cloud_providers": [],
      "config_version": 2,
      "ctx_size": -1,
      "default_model_source": "huggingface",
      "disable_model_filtering": false,
      "download_rate_limit": "",
      "ds4": {"args": ""},
      "enable_dgpu_gtt": false,
      "extra_models_dir": "",
      "flm": {"args": "", "prefer_system": false},
      "global_timeout": 600,
      "host": "localhost",
      "hrx": {"args": "", "hrx_bin": "builtin"},
      "inhibit_suspend": true,
      "kokoro": {"cpu_bin": "builtin"},
      "llamacpp": {
        "args": "",
        "backend": "auto",
        "cpu_args": "",
        "cpu_bin": "builtin",
        "cuda_bin": "builtin",
        "prefer_system": true,
        "rocm_args": "",
        "rocm_bin": "builtin",
        "vulkan_args": "",
        "vulkan_bin": "builtin"
      },
      "log_file": "auto",
      "log_level": "info",
      "log_max_file_size_mb": 10,
      "log_max_files": 5,
      "max_loaded_models": 1,
      "models_dir": "auto",
      "moonshine": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
      "no_fetch_executables": false,
      "offline": false,
      "onnxruntime": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
      "openmoss": {"backend": "auto", "cuda_bin": "builtin", "vulkan_bin": "builtin"},
      "port": 13305,
      "rocm_channel": "stable",
      "rocm_install_method": "auto",
      "ryzenai": {"server_bin": "builtin"},
      "sdcpp": {
        "args": "",
        "backend": "auto",
        "cfg_scale": 7.0,
        "cpu_args": "",
        "cpu_bin": "builtin",
        "cuda_args": "",
        "cuda_bin": "builtin",
        "height": 512,
        "rocm_args": "",
        "rocm_bin": "builtin",
        "steps": 20,
        "vulkan_args": "",
        "vulkan_bin": "builtin",
        "width": 512
      },
      "telemetry": {
        "enabled": false,
        "hide_inputs": false,
        "hide_outputs": false,
        "hide_thinking": false,
        "max_queue_capacity": 1000,
        "otlp": {
          "batch_timeout_s": 1.0,
          "endpoint": "http://localhost:4318/v1/traces",
          "headers": {},
          "max_retries": 0,
          "protocol": "http/protobuf",
          "retry_backoff_base_s": 5.0,
          "semantics": ["openinference", "otel_genai"],
          "send_batch_size": 100
        },
        "session": {"headers": {"client": [], "id": []}},
        "trust_incoming_trace_context": false
      },
      "thenoise": {"backend": "auto", "lora_dir": "", "rocm_bin": "builtin", "upscaler_dir": ""},
      "thinksound": {
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "trellis": {
        "args": "",
        "backend": "auto",
        "cuda_bin": "builtin",
        "rocm_bin": "builtin",
        "vulkan_bin": "builtin"
      },
      "vllm": {"args": "", "backend": "auto"},
      "websocket_port": "auto",
      "whispercpp": {
        "args": "",
        "backend": "auto",
        "cpu_args": "",
        "cpu_bin": "builtin",
        "npu_args": "",
        "npu_bin": "builtin"
      }
    }
    ```

=== "Schema"

    ```json
    {
      "description": "Every config.json key with its factory default.",
      "type": "object",
      "required": ["port", "host", "log_level"],
      "properties": {
        "host": {"type": "string"},
        "log_level": {"type": "string"},
        "port": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: internal.config_defaults -->

<!-- BEGIN GENERATED: internal.cleanup_cache -->
## `POST /internal/cleanup-cache`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Finds model files that a multi-repository model left in another repository's Hugging Face cache folder, and removes them unless `dry_run` is `true`.

An orphan is a non-`main` checkpoint file, such as an `mmproj` from a different repository, found inside the `main` checkpoint's cache folder.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `dry_run` | No | List the orphaned files without removing them. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/cleanup-cache" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"dry_run": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/cleanup-cache \
      -H "Content-Type: application/json" \
      -d '{"dry_run": true}'
    ```

=== "Response"

    `200`

    ```json
    {"dry_run": true, "orphaned_files": [], "total_bytes": 0}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["orphaned_files", "total_bytes", "dry_run"],
      "properties": {
        "dry_run": {"description": "Whether the files were left in place.", "type": "boolean"},
        "orphaned_files": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["path", "size", "model", "type", "belongs_to"],
            "properties": {
              "belongs_to": {"description": "Repository the file belongs to.", "type": "string"},
              "model": {"description": "Model whose checkpoint the file is.", "type": "string"},
              "path": {"description": "Absolute path of the file.", "type": "string"},
              "size": {"description": "Size in bytes.", "type": "integer"},
              "type": {"description": "Checkpoint role, such as mmproj.", "type": "string"}
            }
          }
        },
        "total_bytes": {"description": "Combined size of the orphaned files.", "type": "integer"}
      }
    }
    ```
<!-- END GENERATED: internal.cleanup_cache -->

<!-- BEGIN GENERATED: internal.simulate_vram_pressure -->
## `POST /internal/simulate-vram-pressure`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Runs one eviction-engine evaluation as if global VRAM usage were `pct`, so tests can exercise pressure eviction without filling a GPU.

Only unpinned models with `auto_evict` enabled are candidates. A `pct` at or above `auto_evict_threshold_pct` runs pressure eviction, `-1` runs only the idle-timeout checks, and any other value does nothing.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `pct` | No | Simulated fraction of VRAM in use, e.g. `0.95`. Defaults to `0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/simulate-vram-pressure" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"pct": 0.5}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/simulate-vram-pressure \
      -H "Content-Type: application/json" \
      -d '{"pct": 0.5}'
    ```

=== "Response"

    `200`

    ```json
    {"status": "ok"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "ok"}}}
    ```
<!-- END GENERATED: internal.simulate_vram_pressure -->

<!-- BEGIN GENERATED: internal.models_sync -->
## `POST /internal/models/sync`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Checks downloaded models for newer upstream commits and downloads the updates, by default in the background.

With `async` omitted or `true`, the sync starts in the background and the answer is `202` with the sync's status; poll [`GET /internal/models/sync/status`](#get-internalmodelssyncstatus) with its `sync_id`. With `async: false`, the answer waits for the sync to finish. `dry_run: true` only checks, and answers `200` with what a sync would update.

A sync already running answers with its status instead of starting another. Full offline mode (`offline=true`) answers `409` without any network request.

The `lemonade update-models` command uses this endpoint; see Model Synchronization & Auto-Updates in [Server Configuration](../guide/configuration/README.md).

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `models` | No | Models to sync. Defaults to every downloaded model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | Check for updates without downloading them. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `async` | No | Run in the background and answer immediately. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `attach_if_running` | No | Add the models to a sync that is already running instead of reporting it. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/models/sync" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"models": ["Qwen3-0.6B-GGUF"], "dry_run": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/models/sync \
      -H "Content-Type: application/json" \
      -d '{"models": ["Qwen3-0.6B-GGUF"], "dry_run": true}'
    ```

=== "Response"

    `200`

    ```json
    {
      "already_in_progress": false,
      "checked_count": 1,
      "completed_sync_id": 0,
      "dry_run": true,
      "failed_models": {},
      "models_up_to_date": ["Qwen3-0.6B-GGUF"],
      "models_updated": [],
      "status": "success",
      "sync_id": 0,
      "updated_count": 0
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "sync_id", "completed_sync_id", "already_in_progress"],
      "properties": {
        "active_targets": {"type": "array", "items": {"type": "string"}},
        "already_in_progress": {"description": "Whether a sync is running.", "type": "boolean"},
        "async": {"description": "Background dispatch only: true.", "type": "boolean"},
        "checked_count": {"type": "integer"},
        "completed_sync_id": {
          "description": "The most recent sync that finished.",
          "type": "integer"
        },
        "completed_targets": {"type": "array", "items": {"type": "string"}},
        "dry_run": {"description": "Dry runs only: true.", "type": "boolean"},
        "failed_models": {
          "description": "Error message per model that could not be checked or updated.",
          "type": "object",
          "additionalProperties": {"type": "string"}
        },
        "is_full_sync": {
          "description": "Whether the sync covers every downloaded model.",
          "type": "boolean"
        },
        "message": {"description": "Background dispatch only.", "type": "string"},
        "models_up_to_date": {"type": "array", "items": {"type": "string"}},
        "models_updated": {
          "description": "Models with an update; downloaded unless dry_run.",
          "type": "array",
          "items": {"type": "string"}
        },
        "pending_targets": {"type": "array", "items": {"type": "string"}},
        "progress": {
          "description": "While a sync runs: the file downloading now.",
          "type": "object",
          "properties": {
            "bytes_downloaded": {"type": "integer"},
            "bytes_total": {"type": "integer"},
            "current_model": {"type": "string"},
            "file": {"type": "string"},
            "file_index": {"type": "integer"},
            "percent": {"type": "number"},
            "total_files": {"type": "integer"}
          }
        },
        "status": {"enum": ["idle", "in_progress", "success", "failed", "not_found"]},
        "sync_id": {
          "description": "The sync this status describes; 0 before any sync.",
          "type": "integer"
        },
        "terminal_error": {"type": "string"},
        "updated_count": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: internal.models_sync -->

<!-- BEGIN GENERATED: internal.models_sync_status -->
## `GET /internal/models/sync/status`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the status of a model sync started by [`POST /internal/models/sync`](#post-internalmodelssync), including download progress while it runs.

A `sync_id` that is neither running nor recorded answers `200` with `status: "not_found"`.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `sync_id` (query) | No | Sync to report. Defaults to the latest. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/models/sync/status"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/models/sync/status
    ```

=== "Response"

    `200`

    ```json
    {
      "active_targets": [],
      "already_in_progress": false,
      "checked_count": 0,
      "completed_sync_id": 0,
      "completed_targets": [],
      "failed_models": {},
      "is_full_sync": false,
      "models_up_to_date": [],
      "models_updated": [],
      "pending_targets": [],
      "status": "idle",
      "sync_id": 0,
      "terminal_error": "",
      "updated_count": 0
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "sync_id", "completed_sync_id", "already_in_progress"],
      "properties": {
        "active_targets": {"type": "array", "items": {"type": "string"}},
        "already_in_progress": {"description": "Whether a sync is running.", "type": "boolean"},
        "async": {"description": "Background dispatch only: true.", "type": "boolean"},
        "checked_count": {"type": "integer"},
        "completed_sync_id": {
          "description": "The most recent sync that finished.",
          "type": "integer"
        },
        "completed_targets": {"type": "array", "items": {"type": "string"}},
        "dry_run": {"description": "Dry runs only: true.", "type": "boolean"},
        "failed_models": {
          "description": "Error message per model that could not be checked or updated.",
          "type": "object",
          "additionalProperties": {"type": "string"}
        },
        "is_full_sync": {
          "description": "Whether the sync covers every downloaded model.",
          "type": "boolean"
        },
        "message": {"description": "Background dispatch only.", "type": "string"},
        "models_up_to_date": {"type": "array", "items": {"type": "string"}},
        "models_updated": {
          "description": "Models with an update; downloaded unless dry_run.",
          "type": "array",
          "items": {"type": "string"}
        },
        "pending_targets": {"type": "array", "items": {"type": "string"}},
        "progress": {
          "description": "While a sync runs: the file downloading now.",
          "type": "object",
          "properties": {
            "bytes_downloaded": {"type": "integer"},
            "bytes_total": {"type": "integer"},
            "current_model": {"type": "string"},
            "file": {"type": "string"},
            "file_index": {"type": "integer"},
            "percent": {"type": "number"},
            "total_files": {"type": "integer"}
          }
        },
        "status": {"enum": ["idle", "in_progress", "success", "failed", "not_found"]},
        "sync_id": {
          "description": "The sync this status describes; 0 before any sync.",
          "type": "integer"
        },
        "terminal_error": {"type": "string"},
        "updated_count": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: internal.models_sync_status -->

<!-- BEGIN GENERATED: internal.aliases_list -->
## `GET /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists every model alias with the model it points to.

An alias stands in for its target in any request's `model` field. Aliases live in `<cache_dir>/aliases.json`; see [Model Aliases](../guide/configuration/custom-models.md).

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/aliases"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/aliases
    ```

=== "Response"

    `200`

    ```json
    {
      "aliases": [
        {
          "alias": "my-chat-model",
          "downloaded": true,
          "recipe": "llamacpp",
          "target": "Qwen3-0.6B-GGUF"
        }
      ]
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["aliases"],
      "properties": {
        "aliases": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["alias", "target", "downloaded", "recipe"],
            "properties": {
              "alias": {"type": "string"},
              "downloaded": {
                "description": "Whether the target model is downloaded.",
                "type": "boolean"
              },
              "recipe": {
                "description": "The target model's recipe; llamacpp when the target is not in the registry.",
                "type": "string"
              },
              "target": {"description": "Model or alias the alias points to.", "type": "string"}
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: internal.aliases_list -->

<!-- BEGIN GENERATED: internal.aliases_create -->
## `POST /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Points a model alias at a target model, creating or updating it.

A `400` answers a missing `alias` or `target`, an alias equal to its target, or an alias with a reserved prefix (`user.`, `extra.`, `builtin.`). A `409` answers an alias that is already a model's canonical name, or one that would form a cycle.

The `lemonade alias add` command uses this endpoint.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `alias` | Yes | Alias name to create or update. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `target` | Yes | Model name or canonical ID the alias points to. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/aliases" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"alias": "my-chat-model", "target": "Qwen3-0.6B-GGUF"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/aliases \
      -H "Content-Type: application/json" \
      -d '{"alias": "my-chat-model", "target": "Qwen3-0.6B-GGUF"}'
    ```

=== "Response"

    `200`

    ```json
    {"alias": "my-chat-model", "status": "ok", "target": "Qwen3-0.6B-GGUF"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "alias", "target"],
      "properties": {
        "alias": {"type": "string"},
        "status": {"const": "ok"},
        "target": {"type": "string"}
      }
    }
    ```
<!-- END GENERATED: internal.aliases_create -->

<!-- BEGIN GENERATED: internal.aliases_delete -->
## `DELETE /internal/aliases/{alias}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Removes a model alias. Its target model is unaffected.

An alias that does not exist answers `404` with code `alias_not_found`.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `alias` (path) | Yes | Alias to remove. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/aliases/my-chat-model" `
      -Method DELETE
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/internal/aliases/my-chat-model
    ```

=== "Response"

    `200`

    ```json
    {"alias": "my-chat-model", "status": "deleted"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "alias"],
      "properties": {"alias": {"type": "string"}, "status": {"const": "deleted"}}
    }
    ```
<!-- END GENERATED: internal.aliases_delete -->

<!-- BEGIN GENERATED: internal.routes -->
## `GET /internal/routes`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the specification of every HTTP route lemond serves, in registration order: its URLs, arguments, and the schema and example request of each response format.

`docs/tools/gen_api_boilerplate.py` reads this endpoint to generate the API reference, including this page. See the [HTTP Server Spec](../dev/specs/http-server.md#route-base-classes) for each field.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (query) | No | Return only the route with this id, such as `lemonade.health`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/routes?id=internal.telemetry_flush"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/internal/routes?id=internal.telemetry_flush"
    ```

=== "Response"

    `200`

    ```json
    [
      {
        "args": [],
        "description": "Sends every queued trace span to the configured OTLP collector now, and answers once they are serialized and sent.",
        "experimental": false,
        "id": "internal.telemetry_flush",
        "methods": ["POST"],
        "model_defaults_to_loaded": false,
        "notes": [],
        "paths": ["telemetry/flush"],
        "prefixes": "Internal",
        "quiet_log": false,
        "request_format": "Raw",
        "responses": [
          {
            "example": {},
            "format": "Json",
            "schema": {
              "properties": {"status": {"const": "flushed"}},
              "required": ["status"],
              "type": "object"
            },
            "setup": []
          }
        ],
        "summary": "Force-flush all queued telemetry trace spans",
        "validate_args": false
      }
    ]
    ```

=== "Schema"

    ```json
    {
      "type": "array",
      "items": {
        "type": "object",
        "required": [
          "id",
          "methods",
          "paths",
          "prefixes",
          "summary",
          "description",
          "notes",
          "experimental",
          "args",
          "responses",
          "request_format",
          "model_defaults_to_loaded",
          "validate_args",
          "quiet_log"
        ],
        "properties": {
          "args": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["name", "in", "schema", "required", "supported", "description"],
              "properties": {
                "description": {"type": "string"},
                "in": {"enum": ["JsonBody", "Query", "Path", "Form"]},
                "name": {"type": "string"},
                "required": {"type": "boolean"},
                "schema": {"type": "object"},
                "supported": {
                  "description": "not_available documents an argument Lemonade accepts but ignores; partial, one it honors only in part, as its description says.",
                  "enum": ["available", "partial", "not_available"]
                }
              }
            }
          },
          "description": {"type": "string"},
          "experimental": {"description": "The route may still change.", "type": "boolean"},
          "id": {"description": "<page>.<name>; also the docs anchor.", "type": "string"},
          "methods": {"type": "array", "items": {"type": "string"}},
          "model_defaults_to_loaded": {"type": "boolean"},
          "notes": {"type": "array", "items": {"type": "string"}},
          "paths": {
            "description": "Paths without their prefix; entries after the first are aliases.",
            "type": "array",
            "items": {"type": "string"}
          },
          "prefixes": {
            "description": "Quad serves /api/v0/, /api/v1/, /v0/ and /v1/; Internal serves /internal/; Root serves each path as written.",
            "enum": ["Quad", "Internal", "Root"]
          },
          "quiet_log": {"type": "boolean"},
          "request_format": {"enum": ["Raw", "Json", "OptionalJson", "Form"]},
          "responses": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["format", "schema", "setup", "example"],
              "properties": {
                "example": {"description": "Argument values of a request producing this format."},
                "format": {
                  "enum": [
                    "Json",
                    "JsonLines",
                    "EventStream",
                    "Text",
                    "Binary",
                    "BinaryStream",
                    "Empty"
                  ]
                },
                "schema": {
                  "description": "JSON Schema of the body, each NDJSON line, or each event's data; null for other formats."
                },
                "setup": {
                  "description": "Examples that run first to put lemond in the state this one needs.",
                  "type": "array",
                  "items": {
                    "type": "object",
                    "required": ["route", "format"],
                    "properties": {"format": {"type": "string"}, "route": {"type": "string"}}
                  }
                }
              }
            }
          },
          "summary": {"type": "string"},
          "validate_args": {"type": "boolean"}
        }
      }
    }
    ```
<!-- END GENERATED: internal.routes -->
