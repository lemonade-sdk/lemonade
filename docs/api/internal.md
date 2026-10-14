# Internal API

Internal endpoints control and configure `lemond`. The clients bundled with the server, such as the desktop app, the tray and the `lemonade` CLI, use them, and they accept connections from any address, so a client on another machine can manage a shared `lemond`.

Apps that talk to a system-wide Lemonade Server install must not use these endpoints: that server belongs to its bundled clients, and these endpoints may change without notice. An app that embeds its own `lemond` owns that instance and should use them to manage it; see [Embeddable Lemonade: Runtime](../embeddable/runtime.md).

> **Warning:** When `lemond` is bound to a non-loopback host such as `0.0.0.0`, these endpoints, including shutdown and config, are reachable from the network. `LEMONADE_ADMIN_API_KEY` on its own secures only `/internal/*` and leaves the inference and model-management endpoints under `/v1/` open, so set `LEMONADE_API_KEY` to protect those too. `lemond` logs a startup warning when bound to a non-loopback host without `LEMONADE_API_KEY`.

<!-- BEGIN GENERATED: internal.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/internal/shutdown`](#post-internalshutdown) | Unload every model and stop `lemond` |
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

Unloads every model, answers, and then stops `lemond`.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

Backend processes such as `llama-server` have exited by the time the caller reads the answer. `lemond` then cancels its downloads and exits about 100 ms later.

### Example request

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

### Response format

`200`

```json
{"status": "shutting down"}
```
<!-- END GENERATED: internal.shutdown -->

<!-- BEGIN GENERATED: internal.telemetry_flush -->
## `POST /internal/telemetry/flush`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Sends every queued telemetry trace span to the configured OTLP collector, and answers once they are sent.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

### Example request

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

### Response format

`200`

```json
{"status": "flushed"}
```
<!-- END GENERATED: internal.telemetry_flush -->

<!-- BEGIN GENERATED: internal.pin -->
## `POST /internal/pin`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Pins or unpins a loaded model without reloading it. Least-recently-used eviction skips pinned models.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | Loaded model to pin or unpin. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `pinned` | Yes | `true` pins the model; `false` unpins it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A load that needs a slot held only by pinned models fails with `409` and a `slots_pinned_error` code; see [Model Pinning](../guide/configuration/multi-model.md#model-pinning).

A `400` answers invalid JSON, a missing model name, a missing or non-boolean `pinned`, or a model that is not loaded.

### Example request

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

### Response format

`200`

```json
{"status": "success", "model_name": "Qwen3-0.6B-GGUF", "pinned": true}
```

**Field Descriptions:**

- `model_name` - The model name as sent.
- `pinned` - The model's pinned state now.
<!-- END GENERATED: internal.pin -->

<!-- BEGIN GENERATED: internal.set -->
## `POST /internal/set`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Changes one or more `config.json` settings without restarting `lemond`, and saves them.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `acestep` | No | `acestep` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allowed_origins` | No | Defaults to `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_check_model_updates` | No | Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_evict` | No | Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_evict_threshold_pct` | No | Defaults to `0.9`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auto_update_models` | No | Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `broadcast` | No | Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `config_version` | No | Defaults to `2`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Defaults to `-1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `default_model_source` | No | Defaults to `"huggingface"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `disable_model_filtering` | No | Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `download_rate_limit` | No | Defaults to `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ds4` | No | `ds4` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `enable_dgpu_gtt` | No | Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `extra_models_dir` | No | Defaults to `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `flm` | No | `flm` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `global_timeout` | No | Defaults to `600`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `host` | No | Defaults to `"localhost"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `hrx` | No | `hrx` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `inhibit_suspend` | No | Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `kokoro` | No | `kokoro` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp` | No | `llamacpp` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_file` | No | Defaults to `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_level` | No | Defaults to `"info"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_max_file_size_mb` | No | Defaults to `10`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `log_max_files` | No | Defaults to `5`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_loaded_models` | No | Defaults to `1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `models_dir` | No | Defaults to `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `moonshine` | No | `moonshine` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `no_fetch_executables` | No | Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `offline` | No | Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `onnxruntime` | No | `onnxruntime` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `openmoss` | No | `openmoss` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `port` | No | Defaults to `13305`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `rocm_channel` | No | Defaults to `"stable"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `rocm_install_method` | No | Defaults to `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ryzenai` | No | `ryzenai` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `sdcpp` | No | `sdcpp` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `telemetry` | No | `telemetry` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `thenoise` | No | `thenoise` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `thinksound` | No | `thinksound` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `trellis` | No | `trellis` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vllm` | No | `vllm` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `websocket_port` | No | Defaults to `"auto"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp` | No | `whispercpp` settings; send only the keys to change. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Every key is validated before any is applied, so one invalid value rejects the whole request with `400` and changes nothing.

Settings with a runtime effect apply immediately: a `port` or `host` change rebinds the listeners, `log_level` reconfigures logging, and a backend section's `*_bin` change reinstalls that backend and reloads its models. The rest apply to the next model load or eviction decision. See the [Settings Reference](../guide/configuration/README.md#settings-reference) for every key.

`config.json` keeps only values that differ from the defaults, so setting a key to its default removes it from the file. The `lemonade config set` command uses this endpoint.

### Example request

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

### Response format

`200`

```json
{"status": "success", "updated": {"log_level": "info"}}
```

**Field Descriptions:**

- `updated` - The keys the request set, with their new values.
- `message` - Present when a deprecated value was replaced, such as `rocm_channel=preview`.
<!-- END GENERATED: internal.set -->

<!-- BEGIN GENERATED: internal.config -->
## `GET /internal/config`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the full runtime configuration: every `config.json` key, server-level and per-backend, with its current value.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

The `lemonade config` command reads this endpoint. See the [Settings Reference](../guide/configuration/README.md#settings-reference) for each key.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/config"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/config
    ```

### Response format

`200`

```json
{
  "port": 13305,
  "host": "localhost",
  "log_level": "info",
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
  "log_max_file_size_mb": 10,
  "log_max_files": 5,
  "max_loaded_models": 1,
  "models_dir": "auto",
  "moonshine": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
  "no_fetch_executables": false,
  "offline": false,
  "onnxruntime": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
  "openmoss": {"backend": "auto", "cuda_bin": "builtin", "vulkan_bin": "builtin"},
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
<!-- END GENERATED: internal.config -->

<!-- BEGIN GENERATED: internal.config_defaults -->
## `GET /internal/config/defaults`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the default configuration built into this release, independent of this instance's `config.json` and of any deployment override.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

The per-backend sections come from the backend descriptors, so this is the authoritative list of factory defaults. `docs/tools/gen_backend_boilerplate.py` reads it to regenerate `src/cpp/resources/defaults.json`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/config/defaults"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/config/defaults
    ```

### Response format

`200`

```json
{
  "port": 13305,
  "host": "localhost",
  "log_level": "info",
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
  "log_max_file_size_mb": 10,
  "log_max_files": 5,
  "max_loaded_models": 1,
  "models_dir": "auto",
  "moonshine": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
  "no_fetch_executables": false,
  "offline": false,
  "onnxruntime": {"args": "", "cpu_args": "", "cpu_bin": "builtin"},
  "openmoss": {"backend": "auto", "cuda_bin": "builtin", "vulkan_bin": "builtin"},
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
<!-- END GENERATED: internal.config_defaults -->

<!-- BEGIN GENERATED: internal.cleanup_cache -->
## `POST /internal/cleanup-cache`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Finds model files that a multi-repository model left in another repository's Hugging Face cache folder, and removes them.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `dry_run` | No | List the orphaned files without removing them. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An orphan is a non-`main` checkpoint file, such as an `mmproj` from a different repository, found inside the `main` checkpoint's cache folder.

### Example request

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

### Response format

`200`

```json
{"orphaned_files": [], "total_bytes": 0, "dry_run": true}
```

**Field Descriptions:**

- `orphaned_files`
  - `path` - Absolute path of the file.
  - `size` - Size in bytes.
  - `model` - Model whose checkpoint the file is.
  - `type` - Checkpoint role, such as `mmproj`.
  - `belongs_to` - Repository the file belongs to.
- `total_bytes` - Combined size of the orphaned files.
- `dry_run` - Whether the files were left in place.
<!-- END GENERATED: internal.cleanup_cache -->

<!-- BEGIN GENERATED: internal.simulate_vram_pressure -->
## `POST /internal/simulate-vram-pressure`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Hands the eviction engine a simulated global VRAM usage, so tests can exercise pressure eviction without filling a GPU.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `pct` | No | Simulated fraction of VRAM in use, such as `0.95`. Defaults to `0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Only unpinned models with `auto_evict` enabled are candidates. A `pct` at or above `auto_evict_threshold_pct` runs pressure eviction, `-1` runs only the idle-timeout checks, and any other value does nothing.

The answer comes after the evaluation finishes, so a test can check the outcome right away. While an exclusive job session is active no evaluation runs, and the answer is still `200`.

A `400` answers invalid JSON, a body that is not an object, or a non-numeric `pct`.

### Example request

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

### Response format

`200`

```json
{"status": "ok"}
```
<!-- END GENERATED: internal.simulate_vram_pressure -->

<!-- BEGIN GENERATED: internal.models_sync -->
## `POST /internal/models/sync`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Checks downloaded models for newer upstream commits and downloads the updates.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `models` | No | Models to sync. Defaults to every downloaded model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | Check for updates without downloading them. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `async` | No | Run in the background and answer immediately. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `attach_if_running` | No | Add the models to a sync that is already running instead of reporting it. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A background sync answers `202` with its status; poll [`GET /internal/models/sync/status`](#get-internalmodelssyncstatus) with its `sync_id`.

A `dry_run` finishes before answering, whatever `async` says, and answers `200` with what a sync would update.

While a sync is running, a new request answers `200` with that sync's status instead of starting another.

Offline mode (`offline=true`) answers `409` without any network request.

The `lemonade update-models` command uses this endpoint; see Model Synchronization & Auto-Updates in [Server Configuration](../guide/configuration/README.md).

### Example request

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

### Response format

`200`

```json
{
  "status": "success",
  "sync_id": 0,
  "completed_sync_id": 0,
  "already_in_progress": false,
  "dry_run": true,
  "models_updated": [],
  "models_up_to_date": ["Qwen3-0.6B-GGUF"],
  "failed_models": {},
  "checked_count": 1,
  "updated_count": 0
}
```

**Field Descriptions:**

- `sync_id` - The sync this status describes; `0` before any sync.
- `completed_sync_id` - The most recent sync that finished.
- `already_in_progress` - Whether a sync is running.
- `dry_run` - `true` when the request was a dry run.
- `async` - `true` when the sync was started in the background.
- `message` - Present when the sync was started in the background.
- `is_full_sync` - Whether the sync covers every downloaded model.
- `models_updated` - Models with an update; downloaded unless `dry_run`.
- `failed_models` - Error message per model that could not be checked or updated.
- `progress` - While a sync runs: the file downloading now.
<!-- END GENERATED: internal.models_sync -->

<!-- BEGIN GENERATED: internal.models_sync_status -->
## `GET /internal/models/sync/status`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the status of a model sync started by [`POST /internal/models/sync`](#post-internalmodelssync).

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `sync_id` (query) | No | Sync to report. Defaults to the latest. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `sync_id` that is neither running nor recorded answers `200` with `status: "not_found"`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/models/sync/status"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/models/sync/status
    ```

### Response format

`200`

```json
{
  "status": "idle",
  "sync_id": 0,
  "completed_sync_id": 0,
  "already_in_progress": false,
  "is_full_sync": false,
  "active_targets": [],
  "pending_targets": [],
  "completed_targets": [],
  "models_updated": [],
  "models_up_to_date": [],
  "failed_models": {},
  "terminal_error": "",
  "checked_count": 0,
  "updated_count": 0
}
```

**Field Descriptions:**

- `sync_id` - The sync this status describes; `0` before any sync.
- `completed_sync_id` - The most recent sync that finished.
- `already_in_progress` - Whether a sync is running.
- `dry_run` - `true` when the request was a dry run.
- `async` - `true` when the sync was started in the background.
- `message` - Present when the sync was started in the background.
- `is_full_sync` - Whether the sync covers every downloaded model.
- `models_updated` - Models with an update; downloaded unless `dry_run`.
- `failed_models` - Error message per model that could not be checked or updated.
- `progress` - While a sync runs: the file downloading now.
<!-- END GENERATED: internal.models_sync_status -->

<!-- BEGIN GENERATED: internal.aliases_list -->
## `GET /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists every model alias with its target.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

An alias stands in for its target in any request's `model` field. Aliases live in `<cache_dir>/aliases.json`; see [Model Aliases](../guide/configuration/custom-models.md).

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/aliases"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/aliases
    ```

### Response format

`200`

```json
{
  "aliases": [
    {
      "alias": "my-alias-1",
      "target": "user.custom-llama",
      "downloaded": false,
      "recipe": "llamacpp"
    }
  ]
}
```

**Field Descriptions:**

- `aliases`
  - `alias` - The alias name.
  - `target` - Model or alias the alias points to.
  - `downloaded` - Whether the target model is downloaded.
  - `recipe` - The target model's recipe; `llamacpp` when the target is not in the registry.
<!-- END GENERATED: internal.aliases_list -->

<!-- BEGIN GENERATED: internal.aliases_create -->
## `POST /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Points an alias at a target model, replacing any target it had.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `alias` | Yes | The alias name to create or update. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `target` | Yes | The target model name or canonical ID (also accepted as `model`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `400` answers a missing `alias` or `target`, an alias equal to its target, or an alias with a reserved prefix (`user.`, `extra.`, `builtin.`). A `409` answers an alias that is already a model's canonical name, or one that would form a cycle.

The `lemonade alias add` command uses this endpoint.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/aliases" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"alias": "my-alias-1", "target": "user.custom-llama"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/aliases \
      -H "Content-Type: application/json" \
      -d '{"alias": "my-alias-1", "target": "user.custom-llama"}'
    ```

### Response format

`200`

```json
{"status": "ok", "alias": "my-alias-1", "target": "user.custom-llama"}
```
<!-- END GENERATED: internal.aliases_create -->

<!-- BEGIN GENERATED: internal.aliases_delete -->
## `DELETE /internal/aliases/{alias}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Removes a model alias.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `alias` (path) | Yes | The alias name to remove. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An alias that does not exist answers `404`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/internal/aliases/my-alias-1" `
      -Method DELETE
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/internal/aliases/my-alias-1
    ```

### Response format

`200`

```json
{"status": "deleted", "alias": "my-alias-1"}
```
<!-- END GENERATED: internal.aliases_delete -->

<!-- BEGIN GENERATED: internal.routes -->
## `GET /internal/routes`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the specification of every HTTP route `lemond` serves, in registration order: its URLs, arguments, the schema of each response format, and its example requests.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (query) | No | Return only the route with this id, such as `lemonade.health`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

`docs/tools/gen_api_boilerplate.py` reads this endpoint to generate the API reference, including this page. See the [HTTP Server Spec](../dev/specs/http-server.md#route-base-classes) for each field.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/routes?id=internal.telemetry_flush"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/internal/routes?id=internal.telemetry_flush"
    ```

### Response format

`200`

```json
[
  {
    "id": "internal.telemetry_flush",
    "methods": ["POST"],
    "paths": ["telemetry/flush"],
    "prefixes": "Internal",
    "summary": "Force-flush all queued telemetry trace spans",
    "modality": "",
    "description": "Forces the in-memory telemetry queue to flush all buffered trace spans immediately to the configured OTLP collector. This call blocks until all currently queued spans are serialized and sent.",
    "notes": [],
    "experimental": false,
    "args": [],
    "responses": [
      {
        "format": "Json",
        "schema": {
          "type": "object",
          "required": ["status"],
          "properties": {"status": {"const": "flushed"}}
        }
      }
    ],
    "examples": [{"name": "", "format": "Json", "args": {}, "setup": []}],
    "request_format": "Raw",
    "validate_args": false,
    "quiet_log": false
  }
]
```

**Field Descriptions:**

- `id` - `<page>.<name>`; names the route's generated region in `docs/api/<page>.md`.
- `paths` - Paths without their prefix; entries after the first are aliases.
- `prefixes` - `Quad` serves each path under `/v1/` and older prefixes kept for existing clients; `Internal` under `/internal/`; `Root` as written.
- `modality` - Empty on pages whose summary table has no Modality column.
- `experimental` - The route may still change.
- `args`
  - `supported` - `available`; `partial`, honored only in part as the argument's description says; or `not_available`, accepted but ignored.
- `responses`
  - `schema` - JSON Schema of the body, each NDJSON line, or each event's data; null for other formats.
- `examples` - The first is the route's main example.
  - `name` - Tab label; empty when the route has one example.
  - `format` - The response format the reply is validated against.
  - `args` - Argument values of the request.
  - `setup` - Requests that run first to put `lemond` in the state this example needs.
    - `example` - The name of the route's example to run.
    - `args` - Set instead of example: a request sent as is and left out of the docs.
<!-- END GENERATED: internal.routes -->
