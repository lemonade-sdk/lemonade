# Internal API

Internal endpoints control and configure `lemond`. The clients bundled with the server, such as the desktop app, the tray and the `lemonade` CLI, use them, and they accept connections from any address, so a client on another machine can manage a shared `lemond`.

Apps that talk to a system-wide Lemonade Server install must not use these endpoints: that server belongs to its bundled clients, and these endpoints may change without notice. An app that embeds its own `lemond` owns that instance and should use them to manage it; see [Embeddable Lemonade: Runtime](../embeddable/runtime.md).

> **Warning:** When `lemond` is bound to a non-loopback host such as `0.0.0.0`, these endpoints, including shutdown and config, are reachable from the network. `LEMONADE_ADMIN_API_KEY` on its own secures only `/internal/*` and leaves every other endpoint open, including inference, model management, the Ollama and Anthropic routes and `/mcp`, so set `LEMONADE_API_KEY` to protect those too. `lemond` logs a startup warning when bound to a non-loopback host without `LEMONADE_API_KEY`.

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

### Example

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

**Response:** `200 OK`

```json
{"status": "shutting down"}
```

### Response format

- `status` - Always `shutting down`.
<!-- END GENERATED: internal.shutdown -->

<!-- BEGIN GENERATED: internal.telemetry_flush -->
## `POST /internal/telemetry/flush`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Sends every queued telemetry trace span to the configured OTLP collector, and answers once the queue is empty.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

A batch the collector rejects or does not receive is dropped without further retries, and the answer is still `flushed`.

### Example

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

**Response:** `200 OK`

```json
{"status": "flushed"}
```

### Response format

- `status` - Always `flushed`.
<!-- END GENERATED: internal.telemetry_flush -->

<!-- BEGIN GENERATED: internal.pin -->
## `POST /internal/pin`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Pins or unpins a loaded model without reloading it. Least-recently-used eviction skips pinned models.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` or `model` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `pinned` | Yes | `true` pins the model; `false` unpins it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A [`POST /v1/load`](./lemonade.md#post-v1load) or inference request that needs a slot held only by pinned models fails with a `slots_pinned_error` code; see [Model Pinning](../guide/configuration/multi-model.md#model-pinning).

### Example

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

**Response:** `200 OK`

```json
{"status": "success", "model_name": "Qwen3-0.6B-GGUF", "pinned": true}
```

### Response format

- `status` - Always `success`.
- `model_name` - The model name as sent.
- `pinned` - The model's pinned state now.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The model is not loaded |
<!-- END GENERATED: internal.pin -->

<!-- BEGIN GENERATED: internal.set -->
## `POST /internal/set`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Changes one or more `config.json` settings without restarting `lemond`, and saves them. The `lemonade config set` command uses it.

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

Every key is validated before any is applied, so an unknown key or one invalid value rejects the whole request and changes nothing.

Settings with a runtime effect apply immediately, such as `port` and `host`, which rebind the listeners, the logging settings, and a backend section's `*_bin`, which reinstalls that backend and reloads its models. Other settings are read the next time they are needed, such as at the next model load. See the [Settings Reference](../guide/configuration/README.md#settings-reference) for every key.

`config.json` keeps only values that differ from the defaults, so setting a key to its default removes it from the file.

### Example

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

**Response:** `200 OK`

```json
{"status": "success", "updated": {"log_level": "info"}}
```

### Response format

- `status` - Always `success`.
- `updated` - The keys the request set, with their new values, in `config.json`'s nested form: a flat key such as `llamacpp_backend` appears under `llamacpp`.
- `message` - Present when a deprecated value was replaced, such as `rocm_channel=preview`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The body is `{}`, names an unknown setting, or gives a setting a value outside its range or options |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: internal.set -->

<!-- BEGIN GENERATED: internal.config -->
## `GET /internal/config`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the full runtime configuration: every `config.json` key, server-level and per-backend, with its current value. The `lemonade config` command reads it.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

See the [Settings Reference](../guide/configuration/README.md#settings-reference) for each key.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/config"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/config
    ```

**Response:** `200 OK`

```json
{
  "_generated": "GENERATED by docs/tools/gen_backend_boilerplate.py -- do not hand-edit per-recipe sections (they come from each backend's descriptor config_defaults()). Global keys are hand-maintained in this file. Regenerate and verify with that script; CI --check fails on drift.",
  ... 46 more
}
```

### Response format

Every `config.json` key with its current value.

- A setting; see the Settings Reference.

### Errors

| Status | When |
|--------|------|
| `500` | The request fails unexpectedly |
<!-- END GENERATED: internal.config -->

<!-- BEGIN GENERATED: internal.config_defaults -->
## `GET /internal/config/defaults`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the default configuration built into this release, independent of this instance's `config.json` and of any deployment override. `docs/tools/gen_backend_boilerplate.py` reads it to regenerate `src/cpp/resources/defaults.json`.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

The per-backend sections come from the backend descriptors, so this is the authoritative list of factory defaults.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/config/defaults"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/config/defaults
    ```

**Response:** `200 OK`

```json
{
  "_generated": "GENERATED by docs/tools/gen_backend_boilerplate.py -- do not hand-edit per-recipe sections (they come from each backend's descriptor config_defaults()). Global keys are hand-maintained in this file. Regenerate and verify with that script; CI --check fails on drift.",
  ... 46 more
}
```

### Response format

Every `config.json` key with its factory default.

- A setting; see the Settings Reference.

### Errors

| Status | When |
|--------|------|
| `500` | `resources/defaults.json` is missing or is not valid JSON |
<!-- END GENERATED: internal.config_defaults -->

<!-- BEGIN GENERATED: internal.cleanup_cache -->
## `POST /internal/cleanup-cache`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Finds, and optionally removes, model files that a multi-repository model left in another repository's Hugging Face cache folder.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `dry_run` | No | List the orphaned files without removing them. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An orphan is a non-`main` checkpoint file, such as an `mmproj` from a different repository, found inside the `main` checkpoint's cache folder.

### Example

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

**Response:** `200 OK`

```json
{"orphaned_files": [], "total_bytes": 0, "dry_run": true}
```

### Response format

- `orphaned_files` - Each orphaned file found.
  - `path` - Absolute path of the file.
  - `size` - Size in bytes.
  - `model` - Model whose checkpoint the file is.
  - `type` - Checkpoint role, such as `mmproj`.
  - `belongs_to` - Repository the file belongs to.
- `total_bytes` - Combined size of the orphaned files.
- `dry_run` - Whether the files were left in place.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `500` | `model_not_found` | Scanning or removing cache files fails |
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

Only unpinned models with `auto_evict` enabled are candidates, and a model in use is never chosen. One evaluation unloads at most one model: the first, in load order, past its `evict_idle_timeout`, or when there is none and `pct` is at or above `auto_evict_threshold_pct`, the one with the highest [eviction score](../guide/configuration/multi-model.md#dynamic-vram-management-opt-in). It also downsizes ready models past their `downsize_idle_timeout`. `-1` runs only the idle-timeout checks, and any other value below the threshold does nothing.

The answer comes after the evaluation finishes, so a test can check the outcome right away. While an exclusive job session is active no evaluation runs, and the answer is still `200`.

### Example

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

**Response:** `200 OK`

```json
{"status": "ok"}
```

### Response format

- `status` - Always `ok`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
<!-- END GENERATED: internal.simulate_vram_pressure -->

<!-- BEGIN GENERATED: internal.models_sync -->
## `POST /internal/models/sync`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Checks downloaded models for newer upstream commits and downloads the updates. The `lemonade update-models` command uses it.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `models` | No | Models to sync. Defaults to every downloaded model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `dry_run` | No | `true` reports what the request would do without doing it. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `async` | No | Run in the background and answer immediately. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `attach_if_running` | No | `true` reports a running sync, or with `async: false` waits for it, without adding this request's models to it. Defaults to `false`, which queues the models into the running sync. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A background sync answers `202` with its status; poll [`GET /internal/models/sync/status`](#get-internalmodelssyncstatus) with its `sync_id`.

A `dry_run` finishes before answering, whatever `async` says, and answers `200` with what a sync would update.

While a sync is running, a new request answers `200` with that sync's status instead of starting another.

See Model Synchronization & Auto-Updates in [Server Configuration](../guide/configuration/README.md).

### Example

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

**Response:** `200 OK`

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

### Response format

- `status` - `idle` before any sync, and after a sync other than a dry run that found no models to check; `in_progress` while one runs; `success` or `failed` once it finishes; and `not_found` for an unknown `sync_id`.
- `sync_id` - The sync this status describes; `0` before any sync.
- `completed_sync_id` - The most recent sync that finished.
- `already_in_progress` - Whether a sync this request did not start is running.
- `dry_run` - `true` when the request was a dry run.
- `async` - `true` when the sync was started in the background.
- `message` - Present when the sync was started in the background.
- `is_full_sync` - Whether the sync covers every downloaded model. Absent in a dry run, and when `status` is `not_found`.
- `active_targets` - Models being updated now. Absent in a dry run, and when `status` is `not_found`.
- `pending_targets` - Models queued for the running sync and not yet started. Absent in a dry run, and when `status` is `not_found`.
- `completed_targets` - Models the sync has updated. Absent in a dry run, and when `status` is `not_found`.
- `models_updated` - Models with an update; downloaded unless `dry_run`. Absent when `status` is `not_found`.
- `models_up_to_date` - Models already at the latest upstream commit; absent when `status` is `not_found`.
- `failed_models` - Error message per model that could not be checked or updated; absent when `status` is `not_found`.
- `terminal_error` - Error that stopped the sync; empty when none did. Absent in a dry run, and when `status` is `not_found`.
- `checked_count` - Number of models checked for updates; absent when `status` is `not_found`.
- `updated_count` - Number of models in `models_updated`; absent when `status` is `not_found`.
- `progress` - While a sync runs: the file downloading now.
  - `current_model` - The model being updated.
  - `file` - The file downloading now.
  - `file_index` - Its position among the model's files, from `1`.
  - `total_files` - How many files the model has.
  - `bytes_downloaded` - Bytes of the current file downloaded so far.
  - `bytes_total` - Size of the current file in bytes.
  - `percent` - Share of the current file downloaded, from `0` to `100`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `409` |  | `offline` is set to `true`; no network request is made |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: internal.models_sync -->

<!-- BEGIN GENERATED: internal.models_sync_status -->
## `GET /internal/models/sync/status`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the status of a model sync started by [`POST /internal/models/sync`](#post-internalmodelssync).

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `sync_id` (query) | No | Sync to report. Defaults to the latest. For a sync that has finished, the answer is the status recorded when it finished, `completed_sync_id` and `already_in_progress` included. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `sync_id` that is neither running nor recorded answers `200` with `status: "not_found"`.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/models/sync/status"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/models/sync/status
    ```

**Response:** `200 OK`

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

### Response format

- `status` - `idle` before any sync, and after a sync other than a dry run that found no models to check; `in_progress` while one runs; `success` or `failed` once it finishes; and `not_found` for an unknown `sync_id`.
- `sync_id` - The sync this status describes; `0` before any sync.
- `completed_sync_id` - The most recent sync that finished.
- `already_in_progress` - Whether a sync this request did not start is running.
- `dry_run` - `true` when the request was a dry run.
- `async` - `true` when the sync was started in the background.
- `message` - Present when the sync was started in the background.
- `is_full_sync` - Whether the sync covers every downloaded model. Absent in a dry run, and when `status` is `not_found`.
- `active_targets` - Models being updated now. Absent in a dry run, and when `status` is `not_found`.
- `pending_targets` - Models queued for the running sync and not yet started. Absent in a dry run, and when `status` is `not_found`.
- `completed_targets` - Models the sync has updated. Absent in a dry run, and when `status` is `not_found`.
- `models_updated` - Models with an update; downloaded unless `dry_run`. Absent when `status` is `not_found`.
- `models_up_to_date` - Models already at the latest upstream commit; absent when `status` is `not_found`.
- `failed_models` - Error message per model that could not be checked or updated; absent when `status` is `not_found`.
- `terminal_error` - Error that stopped the sync; empty when none did. Absent in a dry run, and when `status` is `not_found`.
- `checked_count` - Number of models checked for updates; absent when `status` is `not_found`.
- `updated_count` - Number of models in `models_updated`; absent when `status` is `not_found`.
- `progress` - While a sync runs: the file downloading now.
  - `current_model` - The model being updated.
  - `file` - The file downloading now.
  - `file_index` - Its position among the model's files, from `1`.
  - `total_files` - How many files the model has.
  - `bytes_downloaded` - Bytes of the current file downloaded so far.
  - `bytes_total` - Size of the current file in bytes.
  - `percent` - Share of the current file downloaded, from `0` to `100`.

### Errors

| Status | When |
|--------|------|
| `400` | `sync_id` is too large for 64 bits |
| `500` | The request fails unexpectedly |
<!-- END GENERATED: internal.models_sync_status -->

<!-- BEGIN GENERATED: internal.aliases_list -->
## `GET /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists every model alias with its target.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

This endpoint does not take any parameters.

An alias stands in for its target in the `model` field of inference requests and of [`POST /v1/load`](./lemonade.md#post-v1load) and [`POST /v1/unload`](./lemonade.md#post-v1unload); the Ollama and Anthropic gateways do not resolve aliases. Aliases live in `<cache_dir>/aliases.json`; see [Model Aliases](../guide/configuration/custom-models.md).

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/aliases"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/internal/aliases
    ```

**Response:** `200 OK`

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

### Response format

- `aliases` - Each alias with its target.
  - `alias` - The alias name.
  - `target` - Model or alias the alias points to.
  - `downloaded` - Whether the target model is downloaded.
  - `recipe` - The target model's recipe; `llamacpp` when the target is not in the registry.

### Errors

| Status | Code | When |
|--------|------|------|
| `500` | `internal_error` | The alias manager is not initialized, or reading or checking the alias file fails |
<!-- END GENERATED: internal.aliases_list -->

<!-- BEGIN GENERATED: internal.aliases_create -->
## `POST /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Points an alias at a target model, replacing any target it had. The `lemonade alias add` command uses it.

Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise `LEMONADE_API_KEY` when that is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `alias` | Yes | The alias name to create or update. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `target` or `model` | Yes | The model name, canonical ID or other alias to point at, which is not checked to exist. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{"status": "ok", "alias": "my-alias-1", "target": "user.custom-llama"}
```

### Response format

- `status` - Always `ok`.
- `alias` - The alias name.
- `target` - Model or alias the alias now points to.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` | `invalid_request` | `alias` or `target` is blank |
| `400` | `invalid_alias` | `alias` equals `target` |
| `400` | `invalid_alias` | `alias` starts with `user.`, `extra.` or `builtin.` |
| `409` | `alias_conflict` | `alias` is already a registered model's name |
| `409` | `alias_conflict` | The alias would form a cycle or an alias chain deeper than 10 links |
| `500` | `internal_error` | The alias manager is not initialized, or reading or checking the alias file fails |
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

### Example

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

**Response:** `200 OK`

```json
{"status": "deleted", "alias": "my-alias-1"}
```

### Response format

- `status` - Always `deleted`.
- `alias` - The alias that was removed.

### Errors

| Status | Code | When |
|--------|------|------|
| `404` | `alias_not_found` | `alias` names no alias |
| `500` | `internal_error` | The alias manager is not initialized, or reading or checking the alias file fails |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/internal/routes?id=internal.telemetry_flush"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/internal/routes?id=internal.telemetry_flush"
    ```

**Response:** `200 OK`

```json
[
  {
    "id": "internal.telemetry_flush",
    "methods": ["POST"],
    "paths": ["telemetry/flush"],
    "prefixes": "Internal",
    "summary": "Force-flush all queued telemetry trace spans",
    "modality": "",
    "description": "Sends every queued telemetry trace span to the configured OTLP collector, and answers once the queue is empty.",
    "notes": [
      "A batch the collector rejects or does not receive is dropped without further retries, and the answer is still `flushed`."
    ],
    "experimental": false,
    "args": [],
    "responses": [
      {
        "format": "Json",
        "schema": {
          "type": "object",
          "required": ["status"],
          "properties": {"status": {"const": "flushed", "description": "Always `flushed`."}}
        }
      }
    ],
    "examples": [{"name": "", "description": "", "format": "Json", "args": {}, "setup": []}],
    "errors": [],
    "error_format": "Plain",
    "request_format": "Raw",
    "quiet_log": false
  }
]
```

### Response format

- `id` - `<page>.<name>`; names the route's generated region in `docs/api/<page>.md`.
- `methods` - HTTP methods the route answers, such as `POST`.
- `paths` - Paths without their prefix; entries after the first are aliases.
- `prefixes` - `Quad` serves each path under `/v1/` and older prefixes kept for existing clients; `Internal` under `/internal/`; `Root` as written.
- `summary` - One-line summary for the page's summary table; empty when hand-written rows replace it.
- `modality` - The route's input and output for the summary table's Modality column, such as `messages -> completion`; empty on pages without that column.
- `description` - Reference text for the route's section.
- `notes` - Sections the docs place after the Parameters table.
- `experimental` - The route may still change.
- `args` - Every argument the route takes.
  - `name` - The argument's name.
  - `in` - Where the argument goes: the JSON body, the query string, the path, or a form field.
  - `schema` - JSON Schema of the argument's value.
  - `required` - Whether a request must send the argument.
  - `supported` - `available`; `partial`, honored only in part as the argument's description says; or `not_available`, accepted but ignored.
  - `description` - What the argument controls.
  - `aliases` - Other names a request may send the argument under.
  - `forwarded` - Whether `lemond` passes the argument to the backend unchecked.
- `responses` - One entry per response format the route returns.
  - `format` - The response format.
  - `schema` - JSON Schema of the body, each NDJSON line, or each event's data; null for other formats.
- `examples` - The first is the route's main example.
  - `name` - Subsection heading; empty when the route has one example.
  - `description` - What the example shows, opening its subsection; empty when the route's other text covers it.
  - `format` - The response format the reply is validated against.
  - `args` - Argument values of the request.
  - `setup` - Requests that run first to put `lemond` in the state this example needs.
    - `route` - The `id` of the route the step requests.
    - `example` - The name of the route's example to run.
    - `args` - Set instead of example: a request sent as is and left out of the docs.
- `errors` - Every error status the route answers with, including those its base class adds.
  - `status` - The HTTP status.
  - `code` - The code the error body carries, or empty when it carries none.
  - `when` - The condition that produces the error.
- `error_format` - The body shape of the errors `lemond` writes when a request is not valid JSON or an argument is missing or invalid.
- `request_format` - How `lemond` parses the request body: `Raw` not at all, `Json` as JSON, `OptionalJson` as JSON with an empty or `null` body read as `{}`, `Form` as a multipart form.
- `quiet_log` - Whether the route is left out of the access log.
<!-- END GENERATED: internal.routes -->
