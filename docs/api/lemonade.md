# Lemonade API

We have designed a set of Lemonade-specific endpoints to enable client applications by extending the existing cloud-focused APIs (e.g., OpenAI). These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:

- Download models at setup time.
- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Unload models to save memory space.
- Understand system resources and state to make dynamic choices.

<!-- BEGIN GENERATED: lemonade.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/models/check-updates`](#post-v1modelscheck-updates) | Manually check downloaded models for upstream updates |
| `POST` | [`/v1/models/register`](#post-v1modelsregister) | Register or update a user model definition without downloading it |
| `POST` | [`/v1/pull`](#post-v1pull) | Install a model |
| `GET` | [`/v1/pull/variants`](#get-v1pullvariants) | Enumerate GGUF variants for a Hugging Face checkpoint |
| `GET` | [`/v1/registry/search`](#get-v1registrysearch) | Search Hugging Face or ModelScope for model repositories |
| `POST` | [`/v1/delete`](#post-v1delete) | Delete a model |
| `POST` | [`/v1/load`](#post-v1load) | Load a model |
| `POST` | [`/v1/unload`](#post-v1unload) | Unload a model |
| `GET` | [`/v1/downloads`](#get-v1downloads) | List server-owned model download jobs |
| `POST` | [`/v1/downloads/control`](#post-v1downloadscontrol) | Pause, cancel, or remove server-owned model download jobs |
| `POST` | [`/v1/classify`](#post-v1classify) | Classify input text with an encoder classifier (label scores) |
| `POST` | [`/v1/audio/generations`](#post-v1audiogenerations) | Generate audio (music or sound effects) from a text prompt |
| `POST` | [`/v1/3d/generations`](#post-v13dgenerations) | Generate a textured 3D mesh (GLB) from an image |
| `POST` | [`/v1/install`](#post-v1install) | Install or update a backend, or register a cloud provider |
| `POST` | [`/v1/install/dry-run`](#post-v1installdry-run) | Resolve backend install metadata without downloading the backend asset |
| `POST` | [`/v1/uninstall`](#post-v1uninstall) | Remove a backend or cloud provider |
| `POST` | [`/v1/cloud/auth`](#post-v1cloudauth) | Set an in-memory API key for a cloud provider |
| `DELETE` | [`/v1/cloud/auth/{provider}`](#delete-v1cloudauthprovider) | Clear the in-memory API key for a cloud provider |
| `GET` | [`/live`](#get-live) | Check server liveness for load balancers and orchestrators |
| `GET` | [`/metrics`](#get-metrics) | Prometheus metrics scrape endpoint |
| `GET` | [`/v1/health`](#get-v1health) | Check server status, such as models loaded |
| `GET` | [`/v1/docs`](#get-v1docs) | List the API reference pages bundled with the running server |
| `GET` | [`/v1/docs/{page}`](#get-v1docspage) | Read one bundled API reference page |
| `GET` | [`/v1/stats`](#get-v1stats) | Performance statistics from the last request |
| `GET` | [`/v1/system-stats`](#get-v1system-stats) | Current host resource usage |
| `GET` | [`/v1/system-info`](#get-v1system-info) | System information and device enumeration |
| `POST` | [`/v1/jobs`](#post-v1jobs) | Create a job |
| `GET` | [`/v1/jobs`](#get-v1jobs) | List jobs |
| `POST` | [`/v1/jobs/{id}/pause`](#post-v1jobsidpause) | Pause a job |
| `POST` | [`/v1/jobs/{id}/interrupt`](#post-v1jobsidinterrupt) | Interrupt a job |
| `POST` | [`/v1/jobs/{id}/resume`](#post-v1jobsidresume) | Resume a job |
| `GET` | [`/v1/jobs/{id}`](#get-v1jobsid) | Read a job |
| `DELETE` | [`/v1/jobs/{id}`](#delete-v1jobsid) | Delete a job |
<!-- END GENERATED: lemonade.summary -->

Server logs also stream over WebSocket; see [Log Streaming API](#log-streaming-api-websocket). Some Lemonade endpoints live on other pages: the per-model [`/v1/models/{id}/files`](openai.md#get-v1modelsidfiles) and [`/v1/models/{id}/options`](openai.md#get-v1modelsidoptions) extensions sit with the OpenAI models endpoints, routing and [`/v1/routing/validate`](router.md#post-v1routingvalidate) are on the [Router API](router.md) page, and endpoints for clients bundled with the server are on the [Internal API](internal.md) page.

<!-- BEGIN GENERATED: lemonade.models_check_updates -->
## `POST /v1/models/check-updates`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Checks downloaded Hugging Face-backed models for newer upstream commits. It is the manual counterpart to the startup update check and works even when `auto_check_model_updates=false`.

Offline mode remains authoritative: with `offline=true` this answers `409` and makes no network requests.

The CLI runs the same check with `lemonade check-updates`.

Also served at `/api/v0/models/check-updates`, `/api/v1/models/check-updates` and `/v0/models/check-updates`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/models/check-updates" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/models/check-updates
    ```

=== "Response"

    `200`

    ```json
    {"failed_models": {}, "models": [], "status": "success", "updates_available": 0}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "updates_available", "models", "failed_models"],
      "properties": {
        "failed_models": {
          "description": "Each model whose check failed, with its error.",
          "type": "object",
          "additionalProperties": {"type": "string"}
        },
        "models": {
          "description": "Models with a newer upstream commit.",
          "type": "array",
          "items": {"type": "string"}
        },
        "status": {
          "description": "failed when any model's check failed.",
          "enum": ["success", "failed"]
        },
        "updates_available": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: lemonade.models_check_updates -->

<!-- BEGIN GENERATED: lemonade.models_register -->
## `POST /v1/models/register`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Registers or updates a `user.*` model definition without downloading its files, for clients that register and install as separate actions. [`POST /v1/pull`](#post-v1pull) performs the same registration step before downloading.

Registration updates `user_models.json` and invalidates the model cache, but starts no download. A checkpoint is not required, since registration is a metadata operation and some model types have no local weights; `/v1/pull` is what installs.

The endpoint accepts one model definition. A `models` array embedding several definitions is a collection-import concern for `/v1/pull`; register those components first when using this endpoint.

A `400` answers a body that is not a JSON object, a missing `model_name`, a name outside the `user.` namespace or with a reserved `extra.`/`builtin.` prefix, a missing `recipe`, and malformed `checkpoint`, `checkpoints`, `source` or `components` values.

Also served at `/api/v0/models/register`, `/api/v1/models/register` and `/v0/models/register`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | Non-empty name in the `user.` namespace, e.g. `user.Phi-4-Mini-GGUF`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | Yes | Recipe that loads the model, such as `llamacpp`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | Main checkpoint, when the recipe uses one. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Checkpoints by role for multi-checkpoint models; must contain `main`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry or local source. Remote values are `huggingface` and `modelscope`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Additional model labels; see [Model Labels](./openai.md#model-labels). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Collection recipes only: names of already-registered component models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/models/register" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model_name": "user.Phi-4-Mini-GGUF",
          "recipe": "llamacpp",
          "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M"
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/register \
      -H "Content-Type: application/json" \
      -d '{
          "model_name": "user.Phi-4-Mini-GGUF",
          "recipe": "llamacpp",
          "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M"
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "canonical_model_name": "user.Phi-4-Mini-GGUF",
      "model": {
        "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M",
        "checkpoints": {"main": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M"},
        "components": [],
        "created": 1234567890,
        "downloaded": false,
        "id": "Phi-4-Mini-GGUF",
        "labels": ["chat", "custom"],
        "object": "model",
        "owned_by": "lemonade",
        "recipe": "llamacpp",
        "recipe_options": {},
        "registry_source": "huggingface",
        "source": "huggingface",
        "suggested": true,
        "update_available": false
      },
      "model_name": "Phi-4-Mini-GGUF",
      "status": "success"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "model_name", "canonical_model_name"],
      "properties": {
        "canonical_model_name": {"description": "Stable user.* registration id.", "type": "string"},
        "model": {
          "type": "object",
          "required": [
            "id",
            "object",
            "created",
            "owned_by",
            "checkpoint",
            "checkpoints",
            "recipe",
            "downloaded",
            "update_available",
            "suggested",
            "source",
            "registry_source",
            "labels",
            "components",
            "recipe_options"
          ],
          "properties": {
            "audio_defaults": {
              "description": "Audio models only: default generation parameters.",
              "type": "object"
            },
            "checkpoint": {
              "description": "Main checkpoint: a Hugging Face or ModelScope repository and file, or a local path.",
              "type": "string"
            },
            "checkpoints": {
              "description": "Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.",
              "type": "object",
              "additionalProperties": {"type": "string"}
            },
            "cloud_provider": {
              "description": "Cloud models only: the provider that serves the model.",
              "type": "string"
            },
            "components": {
              "description": "Collections only: ordered component model names. Empty for other models.",
              "type": "array",
              "items": {"type": "string"}
            },
            "context_length": {
              "description": "Tokens one request can use: the loaded value when the model is running, else the configured ctx_size or the cloud provider's reported length.",
              "type": "integer"
            },
            "cost_input_per_million": {
              "description": "Cloud models only: USD per million input tokens, when the provider reports it.",
              "type": "number"
            },
            "cost_output_per_million": {
              "description": "Cloud models only: USD per million output tokens, when the provider reports it.",
              "type": "number"
            },
            "created": {
              "description": "Unix timestamp of when the model entry was created.",
              "type": "integer"
            },
            "downloaded": {
              "description": "Whether the model's files are on disk.",
              "type": "boolean"
            },
            "id": {
              "description": "Model identifier, used for loading and inference requests.",
              "type": "string"
            },
            "image_defaults": {
              "description": "Image models only: default generation parameters.",
              "type": "object",
              "properties": {
                "cfg_scale": {
                  "description": "Classifier-free guidance scale, e.g. 1.0 for turbo models and 7.5 for standard models.",
                  "type": "number"
                },
                "flow_shift": {"type": "number"},
                "height": {"description": "Default image height in pixels.", "type": "integer"},
                "sampling_method": {"type": "string"},
                "steps": {
                  "description": "Inference steps, e.g. 4 for turbo models and 20 for standard models.",
                  "type": "integer"
                },
                "width": {"description": "Default image width in pixels.", "type": "integer"}
              }
            },
            "labels": {
              "description": "Capabilities and characteristics; see Model Labels.",
              "type": "array",
              "items": {"type": "string"}
            },
            "max_completion_tokens": {
              "description": "OpenAI's name for max_output_tokens.",
              "type": "integer"
            },
            "max_context_window": {
              "description": "Largest context the model supports, from local metadata. Set for downloaded GGUF models and installed FLM models.",
              "type": "integer"
            },
            "max_output_tokens": {
              "description": "Cloud models only: the most tokens one completion can generate, when the provider reports it.",
              "type": "integer"
            },
            "models": {
              "description": "Collections only: each component's full model object, parallel to components, so an exported collection imports through /v1/pull.",
              "type": "array",
              "items": {"type": "object"}
            },
            "object": {"const": "model"},
            "owned_by": {"const": "lemonade"},
            "recipe": {
              "description": "Backend recipe that loads the model, such as llamacpp, flm or ryzenai-llm.",
              "type": "string"
            },
            "recipe_options": {
              "description": "Options saved for this model in recipe_options.json.",
              "type": "object"
            },
            "registry_source": {
              "description": "Remote registry the checkpoint downloads from: huggingface or modelscope.",
              "type": "string"
            },
            "routing": {
              "description": "Router collections only: the routing policy.",
              "type": "object"
            },
            "size": {"description": "Model size in GB, when known.", "type": "number"},
            "source": {
              "description": "Where the model came from: a registry name, local_upload, local_path or extra_models_dir.",
              "type": "string"
            },
            "suggested": {
              "description": "Whether the model is recommended for general use.",
              "type": "boolean"
            },
            "system_prompt": {
              "description": "Omni collections only: the collection's system prompt override.",
              "type": "string"
            },
            "update_available": {
              "description": "Whether a newer upstream commit exists. Set only for downloaded registry-backed models.",
              "type": "boolean"
            },
            "version": {"description": "Router collections only: the policy document's version."}
          }
        },
        "model_name": {"description": "Public id that /v1/models reports.", "type": "string"},
        "status": {"const": "success"}
      }
    }
    ```
<!-- END GENERATED: lemonade.models_register -->

<!-- BEGIN GENERATED: lemonade.pull -->
## `POST /v1/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Installs a model from the built-in registry, or registers and installs any model from Hugging Face or ModelScope.

A model is registered first, through the same path as [`POST /v1/models/register`](#post-v1modelsregister): a new definition needs a `user.` name, a `recipe`, and a `checkpoint` or a `checkpoints` object with a `main` key. Registration adds the model to `user_models.json` in the Lemonade config directory (default `~/.config/lemonade`), after which `/v1/models` lists it like a built-in model.

Only this endpoint checks a downloaded model's registry for updates, unless `do_not_upgrade` is `true`; loading a model on first use never does.

**Progress:** `stream: true` sends server-sent events: `progress` during each file's download, `complete` when every file is done, and `error`, whose data carries `error`, on failure. With `subscribe: false` as well, the server owns the download instead and answers at once with a job snapshot; clients poll [`GET /v1/downloads`](#get-v1downloads) to restore progress after a reload, tab close or reconnect, and use [`POST /v1/downloads/control`](#post-v1downloadscontrol) to pause, cancel or remove the job.

A `400` answers an invalid definition, an unknown model (with `code: "unknown_model"`), or offline mode (with `code: "lemond_offline"`), and nothing is registered.

Also served at `/api/v0/pull`, `/api/v1/pull` and `/v0/pull`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade model name](https://lemonade-server.ai/models.html) to install, or a `user.`-namespaced name to register and install. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | Registering only: main checkpoint, such as `unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M`. A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Registering only: checkpoints by role (`main`, `mmproj`, `draft`, `text_encoder`, `vae`, ...), for multi-checkpoint models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | No | Registering only: recipe that loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mmproj` | No | Registering only: multimodal projector file for vision models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Registering only: model labels. A model deploys in exactly one [deployment mode](./openai.md#model-labels): naming a mode the recipe cannot serve, or naming two, is rejected with `400`. Omitting the deployment label applies the recipe's default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reasoning` | No | Registering only: adds the `reasoning` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vision` | No | Registering only: adds the `vision` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `embedding` | No | Registering only: adds the `embeddings` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reranking` | No | Registering only: adds the `reranking` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Collections only: ordered, non-empty component model names. Components that are not downloaded yet are pulled by the same call; deleting the collection later removes only its entry, not the components. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `models` | No | Collections only: full model definitions, one per `components` entry. Components not yet registered are registered from them; existing names keep their local definition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry to download from: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `registry_source` | No | Same as `source`; when both are given they must name the same registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `do_not_upgrade` | No | `true` skips checking an already-downloaded model's registry for updates. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Send download progress as server-sent events. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Only with `stream: true`: `false` starts a server-owned download job and answers with its snapshot instead of streaming. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `local_import` | No | Register model files a client already copied into the Hugging Face cache, without downloading. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model_name": "Qwen3-0.6B-GGUF",
          "do_not_upgrade": true,
          "stream": true,
          "subscribe": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{
          "model_name": "Qwen3-0.6B-GGUF",
          "do_not_upgrade": true,
          "stream": true,
          "subscribe": false
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "bytes_downloaded": 0,
      "bytes_previously_downloaded": 0,
      "bytes_total": 0,
      "complete": false,
      "completed_files_bytes": 0,
      "cumulative_bytes_downloaded": 0,
      "file": "",
      "file_index": 0,
      "id": "model:Qwen3-0.6B-GGUF",
      "model_name": "Qwen3-0.6B-GGUF",
      "overall_bytes_downloaded": 0,
      "percent": 0,
      "running": true,
      "status": "downloading",
      "total_download_size": 0,
      "total_files": 0,
      "type": "model"
    }
    ```

=== "Schema"

    ```json
    {
      "oneOf": [
        {
          "type": "object",
          "required": ["status", "model_name"],
          "properties": {
            "message": {"description": "Only for local_import.", "type": "string"},
            "model_name": {"type": "string"},
            "status": {"const": "success"}
          }
        },
        {
          "description": "With stream=true and subscribe=false: the server-owned download job.",
          "type": "object",
          "required": ["id", "type", "model_name", "status", "running"],
          "properties": {
            "bytes_downloaded": {
              "description": "Bytes of the current file downloaded so far.",
              "type": "integer"
            },
            "bytes_previously_downloaded": {
              "description": "Bytes of the current file already on disk when resuming.",
              "type": "integer"
            },
            "bytes_total": {"description": "Size of the current file.", "type": "integer"},
            "code": {
              "description": "unknown_model when the model is not in the registry.",
              "type": "string"
            },
            "complete": {
              "description": "True when the download finished successfully.",
              "type": "boolean"
            },
            "completed_files_bytes": {
              "description": "Bytes of the files finished before the current one.",
              "type": "integer"
            },
            "cumulative_bytes_downloaded": {
              "description": "Bytes downloaded across the whole job.",
              "type": "integer"
            },
            "error": {"description": "Failed jobs only: the error message.", "type": "string"},
            "file": {"description": "File currently downloading.", "type": "string"},
            "file_index": {"type": "integer"},
            "id": {
              "description": "Stable download id: model:<model_name> or backend:<recipe>:<backend>.",
              "type": "string"
            },
            "model_name": {
              "description": "Model name, or recipe:backend for a backend job.",
              "type": "string"
            },
            "overall_bytes_downloaded": {
              "description": "Older name for cumulative_bytes_downloaded.",
              "type": "integer"
            },
            "percent": {"description": "Progress of the current file.", "type": "number"},
            "running": {
              "description": "Whether the worker is still active. A terminal status can still have running=true while the worker releases its files.",
              "type": "boolean"
            },
            "status": {"enum": ["downloading", "paused", "cancelled", "completed", "error"]},
            "total_download_size": {
              "description": "Bytes across all files, when known.",
              "type": "integer"
            },
            "total_files": {"type": "integer"},
            "type": {"description": "What the job downloads.", "enum": ["model", "backend"]}
          }
        }
      ]
    }
    ```

### Response: `EventStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF", "do_not_upgrade": true, "stream": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF", "do_not_upgrade": true, "stream": true}'
    ```

=== "Response"

    `200`

    ```text
    event: complete
    data: {"status": "ok"}
    ```

=== "Schema"

    ```json
    {
      "description": "progress events report one file; the complete event follows the last one, and an error event replaces it on failure.",
      "type": "object",
      "properties": {
        "bytes_downloaded": {"type": "integer"},
        "bytes_previously_downloaded": {"type": "integer"},
        "bytes_total": {"type": "integer"},
        "code": {
          "description": "unknown_model when the model is not in the registry.",
          "type": "string"
        },
        "error": {"description": "Only on an error event.", "type": "string"},
        "file": {"type": "string"},
        "file_index": {"type": "integer"},
        "percent": {"type": "number"},
        "status": {
          "description": "Only on a complete event for an operation that had nothing to download.",
          "const": "ok"
        },
        "total_download_size": {"type": "integer"},
        "total_files": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: lemonade.pull -->

### Install a Model

A model from the built-in registry needs only its name. Without `stream`, the answer arrives when the download finishes:

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{"model_name": "Qwen3-0.6B-GGUF"}'
```

```json
{"model_name": "Qwen3-0.6B-GGUF", "status": "success"}
```

With `"stream": true`, progress arrives as server-sent events while each file downloads, ending with `complete`:

```text
event: progress
data: {"bytes_downloaded":95552525,"bytes_previously_downloaded":0,"bytes_total":382156480,"file":"Qwen3-0.6B-Q4_0.gguf","file_index":1,"percent":25,"total_download_size":382157232,"total_files":2}

event: progress
data: {"bytes_downloaded":191087637,"bytes_previously_downloaded":0,"bytes_total":382156480,"file":"Qwen3-0.6B-Q4_0.gguf","file_index":1,"percent":50,"total_download_size":382157232,"total_files":2}

...

event: complete
data: {"bytes_downloaded":0,"bytes_previously_downloaded":0,"bytes_total":0,"file":"","file_index":2,"percent":100,"total_download_size":0,"total_files":2}
```

### Register a Model While Pulling

A model definition needs a `main` checkpoint, given either as `checkpoint` or as the `main` key of `checkpoints`. Other checkpoint roles depend on the model type. This list is not exhaustive, and may grow as models and backends evolve:

* `mmproj` - used by vision models, if not already embedded in `main`
* `draft` - used by dflash, eagle, and multitoken-prediction, if not already embedded in `main`
* `text_encoder` - text-to-token encoder used by image generation
* `vae` - variational autoencoder used by image generation

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.Phi-4-Mini-GGUF",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M",
    "recipe": "llamacpp"
  }'
```

Instead of `checkpoint` and `mmproj`, a model can be defined with a dict of checkpoint roles. These two requests register the same model:

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.My-Gemma3",
    "checkpoint": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
    "mmproj": "mmproj-model-f16.gguf",
    "vision": true,
    "recipe": "llamacpp"
  }'
```

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.My-Gemma3",
    "checkpoints": {
      "main": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
      "mmproj": "ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf"
    },
    "vision": true,
    "recipe": "llamacpp"
  }'
```

Each backend serves a fixed set of [deployment modes](openai.md#model-labels), and a model deploys in exactly one of them. Naming a mode the recipe cannot serve, or naming two, whether through `labels` or through the `embedding` and `reranking` parameters, is rejected with `400` and nothing is registered:

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{"model_name": "user.Clf", "recipe": "llamacpp",
       "checkpoint": "example/model:Q4_K_M", "labels": ["classification"]}'
```

```json
{"error": "Model 'user.Clf': recipe 'llamacpp' cannot serve 'classification'. It serves 'chat', 'embeddings', 'reranking'. Omit the label to deploy as 'chat'."}
```

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{"model_name": "user.Both", "recipe": "llamacpp",
       "checkpoint": "example/model:Q4_K_M", "labels": ["chat", "embeddings"]}'
```

```json
{"error": "Model 'user.Both': a model deploys in exactly one mode, but these labels name two: 'chat' and 'embeddings'. Register one model per mode."}
```

Omitting the deployment label entirely is always valid: the recipe's default is applied.

### Register an Omni-Model

An omni collection bundles several models into a single entry that can be loaded, pulled, or deleted as a unit. Use `recipe: "collection.omni"` with a `components` array instead of `checkpoint`. Each component must be a regular model. Without a `models` array, every component must already exist in the registry, either built in or a previously registered `user.*` model.

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.MyKit",
    "recipe": "collection.omni",
    "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
  }'
```

### Import an Exported Model File

Files written by `lemonade export` (and the desktop app's Export button) are import-ready `/v1/pull` request bodies: POST the file contents verbatim to register and install the model. This works for regular models and collections alike; exported collection files additionally carry `components` plus a `models` array embedding each component's definition. For the file format and the export, import and Hugging Face workflows, see [Share a collection](../guide/configuration/custom-models.md#share-a-collection-between-machines).

<!-- BEGIN GENERATED: lemonade.pull_variants -->
## `GET /v1/pull/variants`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Inspects a GGUF repository and lists the variants (quantizations and sharded folder groups) available to install. The `lemonade pull <owner/repo>` CLI flow and the desktop app's model search use it to fill in the install form.

Only public registry metadata is read. When the server's environment sets `HF_TOKEN`, it is forwarded as a bearer token so gated repositories can be read.

A `400` answers a missing or malformed `checkpoint`, a `source` that disagrees with the checkpoint URL's registry, or offline mode (`code: "lemond_offline"`). A `404` means the registry has no such repository; other failures answer `500`.

Also served at `/api/v0/pull/variants`, `/api/v1/pull/variants` and `/v0/pull/variants`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `checkpoint` (query) | Yes | Repository id such as `unsloth/Qwen3-8B-GGUF`, or a Hugging Face or ModelScope URL, which is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-0.6B-GGUF"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-0.6B-GGUF"
    ```

=== "Response"

    `200`

    ```json
    {
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF",
      "draft_files": [],
      "mmproj_files": [],
      "recipe": "llamacpp",
      "repo_kind": "gguf",
      "source": "huggingface",
      "suggested_labels": ["chat"],
      "suggested_name": "Qwen3-0.6B-GGUF",
      "variants": [
        {
          "files": ["Qwen3-0.6B-Q4_K_M.gguf"],
          "name": "Q4_K_M",
          "primary_file": "Qwen3-0.6B-Q4_K_M.gguf",
          "sharded": false,
          "size_bytes": 396705472
        },
        {
          "files": ["Qwen3-0.6B-UD-Q4_K_XL.gguf"],
          "name": "UD-Q4_K_XL",
          "primary_file": "Qwen3-0.6B-UD-Q4_K_XL.gguf",
          "sharded": false,
          "size_bytes": 405372608
        },
        {
          "files": ["Qwen3-0.6B-Q8_0.gguf"],
          "name": "Q8_0",
          "primary_file": "Qwen3-0.6B-Q8_0.gguf",
          "sharded": false,
          "size_bytes": 639447744
        },
        {
          "files": ["Qwen3-0.6B-Q4_0.gguf"],
          "name": "Q4_0",
          "primary_file": "Qwen3-0.6B-Q4_0.gguf",
          "sharded": false,
          "size_bytes": 382156480
        },
        {
          "files": ["Qwen3-0.6B-BF16.gguf"],
          "name": "BF16",
          "primary_file": "Qwen3-0.6B-BF16.gguf",
          "sharded": false,
          "size_bytes": 1198182848
        },
        {
          "files": ["Qwen3-0.6B-IQ4_NL.gguf"],
          "name": "IQ4_NL",
          "primary_file": "Qwen3-0.6B-IQ4_NL.gguf",
          "sharded": false,
          "size_bytes": 381566656
        },
        {
          "files": ["Qwen3-0.6B-IQ4_XS.gguf"],
          "name": "IQ4_XS",
          "primary_file": "Qwen3-0.6B-IQ4_XS.gguf",
          "sharded": false,
          "size_bytes": 367804096
        },
        {
          "files": ["Qwen3-0.6B-Q2_K.gguf"],
          "name": "Q2_K",
          "primary_file": "Qwen3-0.6B-Q2_K.gguf",
          "sharded": false,
          "size_bytes": 296238784
        },
        {
          "files": ["Qwen3-0.6B-Q2_K_L.gguf"],
          "name": "Q2_K_L",
          "primary_file": "Qwen3-0.6B-Q2_K_L.gguf",
          "sharded": false,
          "size_bytes": 296238784
        },
        {
          "files": ["Qwen3-0.6B-Q3_K_M.gguf"],
          "name": "Q3_K_M",
          "primary_file": "Qwen3-0.6B-Q3_K_M.gguf",
          "sharded": false,
          "size_bytes": 347127488
        },
        {
          "files": ["Qwen3-0.6B-Q3_K_S.gguf"],
          "name": "Q3_K_S",
          "primary_file": "Qwen3-0.6B-Q3_K_S.gguf",
          "sharded": false,
          "size_bytes": 323075776
        },
        {
          "files": ["Qwen3-0.6B-Q4_1.gguf"],
          "name": "Q4_1",
          "primary_file": "Qwen3-0.6B-Q4_1.gguf",
          "sharded": false,
          "size_bytes": 409091776
        },
        {
          "files": ["Qwen3-0.6B-Q4_K_S.gguf"],
          "name": "Q4_K_S",
          "primary_file": "Qwen3-0.6B-Q4_K_S.gguf",
          "sharded": false,
          "size_bytes": 383270592
        },
        {
          "files": ["Qwen3-0.6B-Q5_K_M.gguf"],
          "name": "Q5_K_M",
          "primary_file": "Qwen3-0.6B-Q5_K_M.gguf",
          "sharded": false,
          "size_bytes": 444415680
        },
        {
          "files": ["Qwen3-0.6B-Q5_K_S.gguf"],
          "name": "Q5_K_S",
          "primary_file": "Qwen3-0.6B-Q5_K_S.gguf",
          "sharded": false,
          "size_bytes": 436616896
        },
        {
          "files": ["Qwen3-0.6B-Q6_K.gguf"],
          "name": "Q6_K",
          "primary_file": "Qwen3-0.6B-Q6_K.gguf",
          "sharded": false,
          "size_bytes": 495107776
        },
        {
          "files": ["Qwen3-0.6B-UD-IQ1_M.gguf"],
          "name": "UD-IQ1_M",
          "primary_file": "Qwen3-0.6B-UD-IQ1_M.gguf",
          "sharded": false,
          "size_bytes": 220754624
        },
        {
          "files": ["Qwen3-0.6B-UD-IQ1_S.gguf"],
          "name": "UD-IQ1_S",
          "primary_file": "Qwen3-0.6B-UD-IQ1_S.gguf",
          "sharded": false,
          "size_bytes": 214643392
        },
        {
          "files": ["Qwen3-0.6B-UD-IQ2_M.gguf"],
          "name": "UD-IQ2_M",
          "primary_file": "Qwen3-0.6B-UD-IQ2_M.gguf",
          "sharded": false,
          "size_bytes": 268702400
        },
        {
          "files": ["Qwen3-0.6B-UD-IQ2_XXS.gguf"],
          "name": "UD-IQ2_XXS",
          "primary_file": "Qwen3-0.6B-UD-IQ2_XXS.gguf",
          "sharded": false,
          "size_bytes": 234074816
        },
        {
          "files": ["Qwen3-0.6B-UD-IQ3_XXS.gguf"],
          "name": "UD-IQ3_XXS",
          "primary_file": "Qwen3-0.6B-UD-IQ3_XXS.gguf",
          "sharded": false,
          "size_bytes": 282088128
        },
        {
          "files": ["Qwen3-0.6B-UD-Q2_K_XL.gguf"],
          "name": "UD-Q2_K_XL",
          "primary_file": "Qwen3-0.6B-UD-Q2_K_XL.gguf",
          "sharded": false,
          "size_bytes": 301727424
        },
        {
          "files": ["Qwen3-0.6B-UD-Q3_K_XL.gguf"],
          "name": "UD-Q3_K_XL",
          "primary_file": "Qwen3-0.6B-UD-Q3_K_XL.gguf",
          "sharded": false,
          "size_bytes": 356622016
        },
        {
          "files": ["Qwen3-0.6B-UD-Q5_K_XL.gguf"],
          "name": "UD-Q5_K_XL",
          "primary_file": "Qwen3-0.6B-UD-Q5_K_XL.gguf",
          "sharded": false,
          "size_bytes": 446381760
        },
        {
          "files": ["Qwen3-0.6B-UD-Q6_K_XL.gguf"],
          "name": "UD-Q6_K_XL",
          "primary_file": "Qwen3-0.6B-UD-Q6_K_XL.gguf",
          "sharded": false,
          "size_bytes": 576467648
        },
        {
          "files": ["Qwen3-0.6B-UD-Q8_K_XL.gguf"],
          "name": "UD-Q8_K_XL",
          "primary_file": "Qwen3-0.6B-UD-Q8_K_XL.gguf",
          "sharded": false,
          "size_bytes": 844288704
        }
      ]
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": [
        "checkpoint",
        "source",
        "recipe",
        "repo_kind",
        "suggested_name",
        "suggested_labels",
        "mmproj_files",
        "draft_files",
        "variants"
      ],
      "properties": {
        "checkpoint": {
          "description": "The repository id, after URL normalization.",
          "type": "string"
        },
        "component_count": {
          "description": "collection only: how many models the collection bundles.",
          "type": "integer"
        },
        "draft_files": {
          "description": "Bare names of draft-model GGUF files.",
          "type": "array",
          "items": {"type": "string"}
        },
        "mmproj_files": {
          "description": "Bare names of mmproj-*.gguf files; pass the first as mmproj to /v1/pull for vision models.",
          "type": "array",
          "items": {"type": "string"}
        },
        "recipe": {
          "description": "Suggested recipe: llamacpp for GGUF repositories, ryzenai-llm for RyzenAI ONNX ones, collection.omni for an exported collection.",
          "type": "string"
        },
        "repo_kind": {"enum": ["gguf", "onnx-ryzenai", "collection"]},
        "size": {
          "description": "collection only: the collection's total size, from its manifest.",
          "type": "number"
        },
        "source": {"description": "Registry the variants came from.", "type": "string"},
        "suggested_labels": {
          "description": "The labels /v1/pull would stamp: the recipe's deployment label (e.g. chat), plus vision when mmproj-*.gguf files exist, mtp or dflash when a draft model ships beside the weights, and embeddings or reranking when those words appear in the repository id.",
          "type": "array",
          "items": {"type": "string"}
        },
        "suggested_name": {
          "description": "Repository id without its owner/ prefix, suitable for a user.<name> model name.",
          "type": "string"
        },
        "variants": {
          "description": "Every quantization in the repository, most common first: Q4_K_M, UD-Q4_K_XL, Q8_0 and Q4_0 lead, then the rest by name. Empty for a collection.",
          "type": "array",
          "items": {
            "type": "object",
            "required": ["name", "primary_file", "files", "sharded", "size_bytes"],
            "properties": {
              "draft_file": {
                "description": "The draft model paired with this variant, when the repository ships one.",
                "type": "string"
              },
              "files": {"type": "array", "items": {"type": "string"}},
              "name": {"description": "e.g. Q4_K_M or UD-Q4_K_XL.", "type": "string"},
              "primary_file": {"type": "string"},
              "sharded": {"type": "boolean"},
              "size_bytes": {"type": "integer"}
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.pull_variants -->

<!-- BEGIN GENERATED: lemonade.registry_search -->
## `GET /v1/registry/search`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Searches a remote model registry (Hugging Face or ModelScope) for repositories matching a query. It returns candidate repositories from registry metadata and does not verify that a repository holds servable files.

The desktop app's Model Manager follows up with [`GET /v1/pull/variants`](#get-v1pullvariants) on each candidate and offers a download only when that file-level validation passes; `has_gguf` is only a hint.

A `400` answers a `query` shorter than 3 characters, an invalid `source`, `limit` or `format`, or offline mode (`code: "lemond_offline"`). A `429` passes on the registry's rate limit, and other upstream transport or parsing failures answer `502`, with the upstream status code when available.

Also served at `/api/v0/registry/search`, `/api/v1/registry/search` and `/v0/registry/search`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` (query) | Yes | Search text, at least 3 characters after trimming. `q` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry to search: `huggingface` (default) or `modelscope`. The aliases `hf` and `ms` are accepted; the response echoes the canonical name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `limit` (query) | No | Most results to return, from 1 to 50. Defaults to 12. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` (query) | No | `gguf`, the only accepted value, favors GGUF repositories in search and ranking. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/registry/search?format=gguf&limit=2&query=qwen3"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/registry/search?format=gguf&limit=2&query=qwen3"
    ```

=== "Response"

    `200`

    ```json
    {
      "format": "gguf",
      "query": "qwen3",
      "results": [
        {
          "description": "",
          "display_name": "Qwen3.8-27B-GGUF",
          "downloads": 6553039,
          "has_gguf": true,
          "likes": 4916,
          "repository_id": "unsloth/Qwen3.8-27B-GGUF",
          "repository_type": "model",
          "source": "huggingface",
          "tags": [
            "gguf",
            "qwen3_5",
            "unsloth",
            "base_model:Qwen/Qwen3.8-27B",
            "base_model:quantized:Qwen/Qwen3.8-27B",
            "license:apache-2.0",
            "endpoints_compatible",
            "region:us",
            "imatrix",
            "conversational"
          ],
          "task": ""
        },
        {
          "description": "",
          "display_name": "Qwen3-Coder-30B-A3B-Instruct-GGUF",
          "downloads": 6499878,
          "has_gguf": true,
          "likes": 1116,
          "repository_id": "unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF",
          "repository_type": "model",
          "source": "huggingface",
          "tags": [
            "transformers",
            "gguf",
            "unsloth",
            "qwen3",
            "qwen",
            "text-generation",
            "arxiv:2505.09388",
            "base_model:Qwen/Qwen3-Coder-30B-A3B-Instruct",
            "base_model:quantized:Qwen/Qwen3-Coder-30B-A3B-Instruct",
            "license:apache-2.0",
            "endpoints_compatible",
            "region:us",
            "imatrix",
            "conversational"
          ],
          "task": "text-generation"
        }
      ],
      "source": "huggingface",
      "total": 2
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["source", "query", "total", "results"],
      "properties": {
        "format": {"description": "Only when format=gguf was requested.", "const": "gguf"},
        "query": {"description": "The trimmed query.", "type": "string"},
        "results": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["repository_id", "display_name", "source"],
            "properties": {
              "description": {"type": "string"},
              "display_name": {"type": "string"},
              "downloads": {"type": "integer"},
              "has_gguf": {
                "description": "Hint from registry metadata that the repository holds GGUF files.",
                "type": "boolean"
              },
              "likes": {"type": "integer"},
              "repository_id": {"type": "string"},
              "repository_type": {"type": "string"},
              "source": {"type": "string"},
              "tags": {"type": "array", "items": {"type": "string"}},
              "task": {"type": "string"}
            }
          }
        },
        "source": {"enum": ["huggingface", "modelscope"]},
        "total": {
          "description": "Matches the registry reported; may exceed the results returned.",
          "type": "integer"
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.registry_search -->

<!-- BEGIN GENERATED: lemonade.delete -->
## `POST /v1/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Deletes a model from local storage, unloading it first if it is loaded.

Deleting a collection (`recipe: "collection.omni"`) removes only the collection entry from `user_models.json`; its components stay on disk. Delete the components individually to free their disk space.

A file still held open, for example by a download that was just cancelled, is retried up to 3 times, 5 seconds apart. An unknown model answers `422`; other failures answer `500`.

Also served at `/api/v0/delete`, `/api/v1/delete` and `/v0/delete`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade model name](https://lemonade-server.ai/models.html) to delete. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/delete" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "user.Phi-4-Mini-GGUF"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/delete \
      -H "Content-Type: application/json" \
      -d '{"model_name": "user.Phi-4-Mini-GGUF"}'
    ```

=== "Response"

    `200`

    ```json
    {"message": "Deleted model: user.Phi-4-Mini-GGUF", "status": "success"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "message"],
      "properties": {"message": {"type": "string"}, "status": {"const": "success"}}
    }
    ```
<!-- END GENERATED: lemonade.delete -->

<!-- BEGIN GENERATED: lemonade.load -->
## `POST /v1/load`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Loads a registered model into memory ahead of the requests that use it, downloading it first if needed.

Recipe options have three states. Omitting one keeps the model's saved value. `null` ignores only that saved value for this load, falling through to the lower default layers without changing `recipe_options.json`. A concrete value overrides the saved one; for `*_args` it replaces the model and architecture args for this load, while backend and machine args remain only when `merge_args` is true. `ctx_size: -1` is a concrete value meaning automatic sizing, not a tombstone. With `save_options: true`, concrete values are persisted and a `null` keeps the existing saved value for its key.

Loading a model that is already loaded with the same options does nothing; different options reload it.

Loading a collection (`recipe: "collection.omni"`) loads each component in turn. Per-model options such as `ctx_size` or `llamacpp_backend` are not forwarded to components; set them on each component's own `recipe_options.json` entry instead. A `collection.router` model loads nothing until a request routes to a candidate.

Every option the model's recipe accepts can be passed; the [Backend Reference](../dev/backends-reference.md#recipe-options) lists them per recipe. Load failures answer with the same error object as inference requests: `404` for an unknown or unsupported model, `409` when pinned models fill every slot, and `500` when the backend fails to start.

Also served at `/api/v0/load`, `/api/v1/load` and `/v0/load`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade model name](https://lemonade-server.ai/models.html) to load. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `pinned` | No | Pin the model so the LRU never evicts it. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `save_options` | No | Save this request's recipe options to `recipe_options.json`, replacing the model's saved values. To save options without loading, or to change one option without resending the rest, use [`POST /v1/models/{id}/options`](./openai.md#post-v1modelsidoptions). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | `llamacpp`, `flm` and `ryzenai-llm`: context size. `-1` sizes it automatically instead of using a saved value. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_backend` | No | `llamacpp`: backend to use, such as `vulkan`, `rocm`, `metal` or `cpu`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_args` | No | `llamacpp`: extra llama-server arguments. `-m`, `--port`, `--ctx-size`, `-ngl`, `--jinja`, `--mmproj`, `--embeddings` and `--reranking` are not allowed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_backend` | No | `whispercpp`: `npu` or `cpu` on Windows, `cpu` or `vulkan` on Linux. Defaults to `npu` where supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_args` | No | `whispercpp`: extra whisper-server arguments, such as `--convert`. `-m`, `--model` and `--port` are not allowed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | `sd-cpp`: inference steps for image generation. Defaults to 20. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | `sd-cpp`: classifier-free guidance scale. Defaults to 7.0. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `width` | No | `sd-cpp`: image width in pixels. Defaults to 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | `sd-cpp`: image height in pixels. Defaults to 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `merge_args` | No | `true` (default) inherits backend and machine `*_args`; a request's `*_args` then replace only the model and architecture args. `false` applies no inherited custom args or overridable runtime defaults. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/load" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF", "llamacpp_args": "--slot-save-path ."}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/load \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF", "llamacpp_args": "--slot-save-path ."}'
    ```

=== "Response"

    `200`

    ```json
    {
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "model_name": "Qwen3-0.6B-GGUF",
      "recipe": "llamacpp",
      "status": "success"
    }
    ```

=== "Schema"

    ```json
    {
      "oneOf": [
        {
          "description": "A model with a backend of its own.",
          "type": "object",
          "required": ["status", "model_name", "checkpoint", "recipe"],
          "properties": {
            "checkpoint": {"type": "string"},
            "model_name": {"type": "string"},
            "recipe": {"type": "string"},
            "status": {"const": "success"}
          }
        },
        {
          "description": "A collection: an Omni collection loads each component; a router collection loads nothing until a request routes to a candidate.",
          "type": "object",
          "required": ["status", "model_name", "recipe"],
          "properties": {
            "model_name": {"type": "string"},
            "recipe": {"enum": ["collection.omni", "collection.router"]},
            "status": {"const": "success"}
          },
          "not": {"required": ["checkpoint"]}
        }
      ]
    }
    ```
<!-- END GENERATED: lemonade.load -->

### Setting Priority

When loading a model, settings are applied in this priority order:

1. Values explicitly passed in the `load` request (highest priority)
2. Per-model values configurable in `recipe_options.json` (see below for details)
3. Values from environment variables or server startup arguments (see [Server Configuration](../guide/configuration/README.md))
4. Default hardcoded values in `lemond` (lowest priority)

### Per-model Options

You can configure recipe-specific options on a per-model basis. Lemonade manages a file called `recipe_options.json` in the user's Lemonade config directory (default: `~/.config/lemonade`). The available options depend on the model's recipe:

```json
{
  "user.Qwen2.5-Coder-1.5B-Instruct": {
    "ctx_size": 16384,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "-np 2 -kvu"
  },
  "Qwen3-Coder-30B-A3B-Instruct-GGUF" : {
    "llamacpp_backend": "rocm"
  },
  "whisper-large-v3-turbo-q8_0.bin": {
    "whispercpp_backend": "npu",
    "whispercpp_args": "--convert"
  }
}
```

Note that model names include any applicable prefix, such as `user.` and `extra.`.

### More Example Requests

Basic load:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{"model_name": "Qwen3-0.6B-GGUF"}'
```

Load with custom settings:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "ctx_size": 8192,
    "llamacpp_backend": "rocm",
    "llamacpp_args": "--flash-attn on --load-mode none"
  }'
```

Load and save settings:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "ctx_size": 8192,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "--no-context-shift --load-mode none",
    "save_options": true
  }'
```

Load a Whisper model with NPU backend and conversion enabled:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "whisper-large-v3-turbo-q8_0.bin",
    "whispercpp_backend": "npu",
    "whispercpp_args": "--convert"
  }'
```

Load an image generation model with custom settings:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "sd-turbo",
    "steps": 4,
    "cfg_scale": 1.0,
    "width": 512,
    "height": 512
  }'
```

<!-- BEGIN GENERATED: lemonade.unload -->
## `POST /v1/unload`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Unloads a model, or every model, from memory, freeing it while the server keeps running.

The body is optional: an empty or unparsable body unloads every model. A model that is not loaded answers `404`.

Also served at `/api/v0/unload`, `/api/v1/unload` and `/v0/unload`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | No | Model to unload; `model` is accepted as an alias. Omit it to unload every model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/unload" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/unload \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF"}'
    ```

=== "Response"

    `200`

    ```json
    {"message": "Model unloaded successfully", "model_name": "Qwen3-0.6B-GGUF", "status": "success"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["status", "message"],
      "properties": {
        "message": {"type": "string"},
        "model_name": {"description": "Only when one model was unloaded.", "type": "string"},
        "status": {"const": "success"}
      }
    }
    ```
<!-- END GENERATED: lemonade.unload -->

<!-- BEGIN GENERATED: lemonade.downloads -->
## `GET /v1/downloads`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the server-owned download jobs that [`POST /v1/pull`](#post-v1pull) and [`POST /v1/install`](#post-v1install) start with `stream: true` and `subscribe: false`, so a client can restore its download manager after a reload or reconnect.

Active, paused, cancelled and failed jobs stay listed until a client removes them with [`POST /v1/downloads/control`](#post-v1downloadscontrol). Completed jobs stay listed for 30 seconds, so clients can observe completion and refresh model state.

Also served at `/api/v0/downloads`, `/api/v1/downloads` and `/v0/downloads`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/downloads"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/downloads
    ```

=== "Response"

    `200`

    ```json
    [
      {
        "bytes_downloaded": 0,
        "bytes_previously_downloaded": 0,
        "bytes_total": 0,
        "complete": true,
        "completed_files_bytes": 0,
        "cumulative_bytes_downloaded": 0,
        "file": "",
        "file_index": 0,
        "id": "model:Qwen3-0.6B-GGUF",
        "model_name": "Qwen3-0.6B-GGUF",
        "overall_bytes_downloaded": 0,
        "percent": 100,
        "running": false,
        "status": "completed",
        "total_download_size": 0,
        "total_files": 0,
        "type": "model"
      }
    ]
    ```

=== "Schema"

    ```json
    {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id", "type", "model_name", "status", "running"],
        "properties": {
          "bytes_downloaded": {
            "description": "Bytes of the current file downloaded so far.",
            "type": "integer"
          },
          "bytes_previously_downloaded": {
            "description": "Bytes of the current file already on disk when resuming.",
            "type": "integer"
          },
          "bytes_total": {"description": "Size of the current file.", "type": "integer"},
          "code": {
            "description": "unknown_model when the model is not in the registry.",
            "type": "string"
          },
          "complete": {
            "description": "True when the download finished successfully.",
            "type": "boolean"
          },
          "completed_files_bytes": {
            "description": "Bytes of the files finished before the current one.",
            "type": "integer"
          },
          "cumulative_bytes_downloaded": {
            "description": "Bytes downloaded across the whole job.",
            "type": "integer"
          },
          "error": {"description": "Failed jobs only: the error message.", "type": "string"},
          "file": {"description": "File currently downloading.", "type": "string"},
          "file_index": {"type": "integer"},
          "id": {
            "description": "Stable download id: model:<model_name> or backend:<recipe>:<backend>.",
            "type": "string"
          },
          "model_name": {
            "description": "Model name, or recipe:backend for a backend job.",
            "type": "string"
          },
          "overall_bytes_downloaded": {
            "description": "Older name for cumulative_bytes_downloaded.",
            "type": "integer"
          },
          "percent": {"description": "Progress of the current file.", "type": "number"},
          "running": {
            "description": "Whether the worker is still active. A terminal status can still have running=true while the worker releases its files.",
            "type": "boolean"
          },
          "status": {"enum": ["downloading", "paused", "cancelled", "completed", "error"]},
          "total_download_size": {
            "description": "Bytes across all files, when known.",
            "type": "integer"
          },
          "total_files": {"type": "integer"},
          "type": {"description": "What the job downloads.", "enum": ["model", "backend"]}
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.downloads -->

<!-- BEGIN GENERATED: lemonade.downloads_control -->
## `POST /v1/downloads/control`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Pauses, cancels or removes a server-owned download job.

`pause` asks the worker to stop and keeps the job listed as `paused`. `cancel` asks it to stop and marks the job `cancelled`; clients should wait for `running: false` before deleting partial files. Either may briefly report `running: true` while the worker unwinds, and a job that already finished is left as it is.

`remove` drops a stopped job from the list. While its worker is still running, the job stays listed and the request is treated as a `cancel` until the worker stops. Removing a job that is not listed succeeds with `missing: true`.

A `400` answers a missing `id` or `action`, invalid JSON, or an unknown action. A `pause` or `cancel` for a job that is not listed answers `404`.

Also served at `/api/v0/downloads/control`, `/api/v1/downloads/control` and `/v0/downloads/control`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` | Yes | Download id from `POST /v1/pull` or `GET /v1/downloads`, e.g. `model:Qwen3-0.6B-GGUF`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `action` | Yes | `pause`, `cancel` or `remove`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/downloads/control" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"id": "model:Qwen3-0.6B-GGUF", "action": "remove"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/downloads/control \
      -H "Content-Type: application/json" \
      -d '{"id": "model:Qwen3-0.6B-GGUF", "action": "remove"}'
    ```

=== "Response"

    `200`

    ```json
    {"status": "ok"}
    ```

=== "Schema"

    ```json
    {
      "oneOf": [
        {
          "type": "object",
          "required": ["status"],
          "properties": {
            "missing": {"description": "Only when the job was not listed.", "const": true},
            "status": {"const": "ok"}
          }
        },
        {
          "description": "pause, cancel, and remove of a running job: the job's snapshot.",
          "type": "object",
          "required": ["id", "type", "model_name", "status", "running"],
          "properties": {
            "bytes_downloaded": {
              "description": "Bytes of the current file downloaded so far.",
              "type": "integer"
            },
            "bytes_previously_downloaded": {
              "description": "Bytes of the current file already on disk when resuming.",
              "type": "integer"
            },
            "bytes_total": {"description": "Size of the current file.", "type": "integer"},
            "code": {
              "description": "unknown_model when the model is not in the registry.",
              "type": "string"
            },
            "complete": {
              "description": "True when the download finished successfully.",
              "type": "boolean"
            },
            "completed_files_bytes": {
              "description": "Bytes of the files finished before the current one.",
              "type": "integer"
            },
            "cumulative_bytes_downloaded": {
              "description": "Bytes downloaded across the whole job.",
              "type": "integer"
            },
            "error": {"description": "Failed jobs only: the error message.", "type": "string"},
            "file": {"description": "File currently downloading.", "type": "string"},
            "file_index": {"type": "integer"},
            "id": {
              "description": "Stable download id: model:<model_name> or backend:<recipe>:<backend>.",
              "type": "string"
            },
            "model_name": {
              "description": "Model name, or recipe:backend for a backend job.",
              "type": "string"
            },
            "overall_bytes_downloaded": {
              "description": "Older name for cumulative_bytes_downloaded.",
              "type": "integer"
            },
            "percent": {"description": "Progress of the current file.", "type": "number"},
            "running": {
              "description": "Whether the worker is still active. A terminal status can still have running=true while the worker releases its files.",
              "type": "boolean"
            },
            "status": {"enum": ["downloading", "paused", "cancelled", "completed", "error"]},
            "total_download_size": {
              "description": "Bytes across all files, when known.",
              "type": "integer"
            },
            "total_files": {"type": "integer"},
            "type": {"description": "What the job downloads.", "enum": ["model", "backend"]}
          }
        }
      ]
    }
    ```
<!-- END GENERATED: lemonade.downloads_control -->

<!-- BEGIN GENERATED: lemonade.classify -->
## `POST /v1/classify`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Runs an encoder text classifier, such as a PII, prompt-safety or domain classifier, on a string and returns a score in `[0, 1]` for each label.

The model must use the `onnxruntime` recipe; see [Classifier Models](#classifier-models) for the architectures it serves. Both sequence-classification models (one label set) and token-classification models (aggregated span labels) are supported.

Label names come from the model's `id2label`, in `config.json` or in a `manifest.json` that overrides it. Some upstream models only declare generic `LABEL_<n>` names; see the model card for their meaning.

A malformed request (invalid JSON, a missing `input`, a non-string field, or a `top_k` that is not a positive integer) is answered with `400` before any model is loaded.

Also served at `/api/v0/classify`, `/api/v1/classify` and `/v0/classify`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | No | Classifier model, using the `onnxruntime` recipe; loaded on first use. Optional when exactly one classification model is loaded, which then serves the request. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to classify. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `text` | No | Alias for `input`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Return only the `top_k` highest-scoring labels. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/classify" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Phishing-Email-Detection-ONNX",
          "input": "Please verify your account at http://secure-login.example now."
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/classify \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Phishing-Email-Detection-ONNX",
          "input": "Please verify your account at http://secure-login.example now."
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "labels": {
        "LABEL_0": 5.558096745517105e-05,
        "LABEL_1": 0.9999443888664246,
        "LABEL_2": 7.880903929446959e-09,
        "LABEL_3": 2.331440818181818e-08
      },
      "model": "Phishing-Email-Detection-ONNX",
      "object": "classification"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["object", "model", "labels"],
      "properties": {
        "labels": {
          "description": "Score in [0, 1] for each label.",
          "type": "object",
          "additionalProperties": {"type": "number"}
        },
        "model": {
          "description": "The model that classified the input, including one chosen because it was the only one loaded.",
          "type": "string"
        },
        "object": {"const": "classification"}
      }
    }
    ```
<!-- END GENERATED: lemonade.classify -->

### Classifier Models

**Supported architectures:** single-sequence encoder families: BERT, DistilBERT, RoBERTa, XLM-RoBERTa, DeBERTa (v1/v2), ELECTRA, ALBERT, CamemBERT. A stock `optimum-cli export onnx` directory of one of these works as is.

A servable model directory is `model.onnx` + `tokenizer.json` + `config.json`. The `config.json` is **always required**: it declares the architecture, which is checked against the list above so an unsupported family (e.g. XLNet, which uses different segment/special-token conventions) is **rejected at load time** rather than served with wrong scores. The output contract (labels, normalization, token budget) is read from that same config; an optional `manifest.json` overrides it but does not replace the config. Without a manifest, inference assumes **single-label softmax**; a multi-label (sigmoid) model must declare `problem_type: multi_label_classification` in its config or ship a `manifest.json`.

<!-- BEGIN GENERATED: lemonade.audio_generations -->
## `POST /v1/audio/generations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates an audio clip from a text prompt. The model decides the kind of audio: music with ACE-Step models (e.g. `ACE-Step-Music`), sound effects with ThinkSound models (e.g. `ThinkSound-SFX`).

This is a Lemonade extension: OpenAI's audio endpoints cover only speech and transcription.

**Performance:** generation runs on the GPU (Vulkan, ROCm or CUDA) and takes from seconds for short sound effects to minutes for full-length music.

A failure is answered with a JSON `error` object instead of audio: `400` for an invalid request, `404` for an unknown model, `500` when the backend reports an error, and `502` when the backend produces no output.

Also served at `/api/v0/audio/generations`, `/api/v1/audio/generations` and `/v0/audio/generations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Audio-generation model, e.g. `ThinkSound-SFX` or `ACE-Step-Music`; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Description of the music or sound effect. For music, this is the style: genre, mood, tempo, instruments and voice. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `lyrics` | No | ACE-Step only: lyrics to sing. Omitted, empty, or `[Instrumental]` (any case) produces an instrumental track; see [Lyrics](#lyrics) for the format. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vocal_language` | No | ACE-Step only: BCP-47 language code of the lyrics, e.g. `en`, `fr` or `ja`. Defaults to `en`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `duration` | No | Length of the clip in seconds. Defaults to the backend's own default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Inference steps. Fewer is faster; more can improve quality. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg` | No | ThinkSound only: classifier-free guidance strength. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed, for reproducible output. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only formats the backend produces natively are accepted (currently `wav`); any other is answered with `400`. Defaults to `wav`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Binary`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/generations" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "ThinkSound-SFX",
          "prompt": "glass shattering on a stone floor",
          "duration": 2,
          "steps": 8,
          "seed": 42
        }' `
      -OutFile output.wav
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/audio/generations \
      -H "Content-Type: application/json" \
      -d '{
          "model": "ThinkSound-SFX",
          "prompt": "glass shattering on a stone floor",
          "duration": 2,
          "steps": 8,
          "seed": 42
        }' \
      --output output.wav
    ```

=== "Response"

    `200`, `Content-Type: audio/wav`, 352,300 bytes
<!-- END GENERATED: lemonade.audio_generations -->

### Lyrics

ACE-Step vocals are a two-stage pipeline inside the backend: a language model first turns the style description and lyrics into audio codes, then the diffusion synthesizer renders those codes into audio. The instrumental path skips the language-model stage entirely, which also means lyrics embedded in the `prompt` field are treated as style text and are never sung. Vocal generations take noticeably longer than instrumental ones of the same duration because of the extra language-model pass.

Format the `lyrics` value the way the ACE-Step authors recommend:

- Mark each song section with a structure tag on its own line: `[verse]`, `[chorus]`, `[bridge]`, `[intro]`, `[outro]`.
- Write one sung phrase per line and separate sections with a blank line.
- Describe the voice ("gentle female vocals", "raspy male baritone") in `prompt`, not in the lyrics.
- Lyrics may be in any supported language; set `vocal_language` to match.

Music with vocals:

```bash
curl -X POST http://localhost:13305/v1/audio/generations \
  -H "Content-Type: application/json" \
  -d '{
        "model": "ACE-Step-Music",
        "prompt": "warm acoustic folk ballad, fingerpicked guitar, gentle female vocals",
        "lyrics": "[verse]\nMoonlight spills across the floor\nShadows dancing by the door\n\n[chorus]\nWe sing until the morning light\nCarried on the wind tonight",
        "duration": 60
      }' \
  --output song.wav
```

<!-- BEGIN GENERATED: lemonade.3d_generations -->
## `POST /v1/3d/generations`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Reconstructs a textured 3D mesh from an image and returns it as a glTF-binary (`.glb`) file. TRELLIS models, such as `TRELLIS-3D`, serve it.

This is a Lemonade extension; OpenAI has no 3D endpoint.

**Performance:** reconstruction runs on the GPU (Vulkan, ROCm or CUDA) and takes minutes; higher resolutions take longer.

A failure is answered with a JSON `error` object instead of a mesh: `400` for an invalid request, checked before the model loads, `404` for an unknown model, `500` when the backend reports an error, and `502` when the backend produces no output.

Also served at `/api/v0/3d/generations`, `/api/v1/3d/generations` and `/v0/3d/generations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | 3D-generation model, e.g. `TRELLIS-3D`; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | Base64-encoded input image, optionally as a `data:` URL. PNG, JPEG, BMP and GIF are accepted. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `resolution` | No | Cascade resolution: `512`, `1024` or `1536`. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `bg_removal` | No | Background removal mode: `threshold`, or `birefnet` for photos with real backgrounds. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `uv` | No | UV atlas method. `xatlas` (default) runs a full UV unwrap that gives every face its own atlas space: the best quality, but its chart computation grows faster than the face count. `box` is a faster 6-plane projection with occlusion-aware bucket assignment and depth-tested rasterization; small texture artifacts remain possible in concave regions. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed, for reproducible output. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only formats the backend produces natively are accepted (currently `glb`); any other is answered with `400`. Defaults to `glb`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Binary`

=== "PowerShell"

    ```powershell
    $image = [Convert]::ToBase64String([IO.File]::ReadAllBytes("image.png"))
    $body = @"
    {"model": "TRELLIS-3D", "image": "$image", "resolution": 512, "seed": 42}
    "@
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/3d/generations" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body $body `
      -OutFile output.glb
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/3d/generations \
      -H "Content-Type: application/json" \
      -d "{\"model\": \"TRELLIS-3D\", \"image\": \"$(base64 -w0 image.png)\", \"resolution\": 512, \"seed\": 42}" \
      --output output.glb
    ```

=== "Response"

    `200`, `Content-Type: model/gltf-binary`, 4,534,160 bytes
<!-- END GENERATED: lemonade.3d_generations -->

<!-- BEGIN GENERATED: lemonade.install -->
## `POST /v1/install`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Installs or updates the backend for a recipe, or registers a cloud provider when `backend` is `"cloud"`.

**Status:** the cloud-provider branch is experimental.

A local backend that is already installed but outdated is updated to the configured version. A backend this system does not support is refused with `400` unless `force` is set, and one that needs manual setup answers with the setup guide's URL instead of installing. See [Install a Cloud Provider](#install-a-cloud-provider) for the cloud branch.

`stream: true` sends progress as server-sent events; with `subscribe: false` as well, the server owns the download, which [`GET /v1/downloads`](#get-v1downloads) and [`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control.

A local install without both `recipe` and `backend`, or a backend this system does not support, is answered with `400` and an `error` string; an invalid cloud provider field is answered with `400` and an `error` object. Any other failure, including invalid JSON, is answered with `500` and an `error` string.

Also served at `/api/v0/install`, `/api/v1/install` and `/v0/install`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: recipe name, e.g. `llamacpp`, `flm`, `whispercpp`, `sd-cpp` or `ryzenai-llm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Backend within the recipe, e.g. `vulkan`, `rocm`, `cpu` or `default`, or `"cloud"` to register a cloud provider. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Local backends: send progress as server-sent events. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Local backends with `stream: true`: `false` starts a server-owned download and answers with its snapshot at once. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `force` | No | Local backends: install even when this system does not support the backend. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, required: short name, e.g. `fireworks`, matching `[a-z0-9_-]+`. It prefixes the provider's model names. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `base_url` | No | Cloud providers, required: base URL, usually ending in `/v1`, saved to `config.json`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | No | Cloud providers: API key, held in lemond's memory only. An environment variable for the provider takes precedence; see [`POST /v1/cloud/auth`](#post-v1cloudauth). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Cloud providers: must be `true` to send an API key to an `http://` base URL. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_name` | No | Cloud providers: header that carries the API key. Defaults to `Authorization`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_prefix` | No | Cloud providers: text before the key in that header. Defaults to `"Bearer "`; pass `""` for gateways that expect the bare key. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `wire_format` | No | Cloud providers: `openai` (default), or `anthropic` for a provider served only from `POST /v1/messages`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"backend": "cloud", "provider": "example", "base_url": "https://api.example.com/v1"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/install \
      -H "Content-Type: application/json" \
      -d '{"backend": "cloud", "provider": "example", "base_url": "https://api.example.com/v1"}'
    ```

=== "Response"

    `200`

    ```json
    {
      "allow_insecure_http": false,
      "auth_header_name": "Authorization",
      "auth_header_prefix": "Bearer ",
      "auth_state": {"env_var_set": false, "runtime_key_set": false},
      "backend": "cloud",
      "base_url": "https://api.example.com/v1",
      "models_discovered": 0,
      "provider": "example",
      "status": "success",
      "wire_format": "openai"
    }
    ```

=== "Schema"

    ```json
    {
      "oneOf": [
        {
          "description": "A local backend installed or already up to date.",
          "type": "object",
          "required": ["status", "recipe", "backend"],
          "properties": {
            "backend": {"type": "string"},
            "recipe": {"type": "string"},
            "status": {"const": "success"}
          }
        },
        {
          "description": "A backend that needs manual setup on this system, such as FLM on Linux: nothing was installed and action is the setup guide's URL.",
          "type": "object",
          "required": ["action", "recipe", "backend"],
          "properties": {
            "action": {"type": "string"},
            "backend": {"type": "string"},
            "recipe": {"type": "string"}
          }
        },
        {
          "description": "A cloud provider registered.",
          "type": "object",
          "required": [
            "status",
            "backend",
            "provider",
            "base_url",
            "allow_insecure_http",
            "auth_header_name",
            "auth_header_prefix",
            "wire_format",
            "models_discovered",
            "auth_state"
          ],
          "properties": {
            "allow_insecure_http": {"type": "boolean"},
            "auth_header_name": {"type": "string"},
            "auth_header_prefix": {"type": "string"},
            "auth_state": {
              "type": "object",
              "properties": {
                "env_var_set": {
                  "description": "Whether LEMONADE_<PROVIDER>_API_KEY is set for lemond.",
                  "type": "boolean"
                },
                "runtime_key_set": {
                  "description": "Whether a key is held in lemond's memory.",
                  "type": "boolean"
                }
              }
            },
            "backend": {"const": "cloud"},
            "base_url": {"type": "string"},
            "models_discovered": {
              "description": "Chat models discovered with the resolved API key; 0 when no key resolves.",
              "type": "integer"
            },
            "provider": {"type": "string"},
            "status": {"const": "success"},
            "warning": {
              "description": "The warnings joined into one string, for older clients.",
              "type": "string"
            },
            "warnings": {"type": "array", "items": {"type": "string"}},
            "wire_format": {"enum": ["openai", "anthropic"]}
          }
        },
        {
          "description": "stream=true with subscribe=false: the server-owned download job just started.",
          "type": "object",
          "required": ["id", "type", "model_name", "status", "running"],
          "properties": {
            "bytes_downloaded": {
              "description": "Bytes of the current file downloaded so far.",
              "type": "integer"
            },
            "bytes_previously_downloaded": {
              "description": "Bytes of the current file already on disk when resuming.",
              "type": "integer"
            },
            "bytes_total": {"description": "Size of the current file.", "type": "integer"},
            "code": {
              "description": "unknown_model when the model is not in the registry.",
              "type": "string"
            },
            "complete": {
              "description": "True when the download finished successfully.",
              "type": "boolean"
            },
            "completed_files_bytes": {
              "description": "Bytes of the files finished before the current one.",
              "type": "integer"
            },
            "cumulative_bytes_downloaded": {
              "description": "Bytes downloaded across the whole job.",
              "type": "integer"
            },
            "error": {"description": "Failed jobs only: the error message.", "type": "string"},
            "file": {"description": "File currently downloading.", "type": "string"},
            "file_index": {"type": "integer"},
            "id": {
              "description": "Stable download id: model:<model_name> or backend:<recipe>:<backend>.",
              "type": "string"
            },
            "model_name": {
              "description": "Model name, or recipe:backend for a backend job.",
              "type": "string"
            },
            "overall_bytes_downloaded": {
              "description": "Older name for cumulative_bytes_downloaded.",
              "type": "integer"
            },
            "percent": {"description": "Progress of the current file.", "type": "number"},
            "running": {
              "description": "Whether the worker is still active. A terminal status can still have running=true while the worker releases its files.",
              "type": "boolean"
            },
            "status": {"enum": ["downloading", "paused", "cancelled", "completed", "error"]},
            "total_download_size": {
              "description": "Bytes across all files, when known.",
              "type": "integer"
            },
            "total_files": {"type": "integer"},
            "type": {"description": "What the job downloads.", "enum": ["model", "backend"]}
          }
        }
      ]
    }
    ```

### Response: `EventStream`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"recipe": "llamacpp", "backend": "vulkan", "stream": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/install \
      -H "Content-Type: application/json" \
      -d '{"recipe": "llamacpp", "backend": "vulkan", "stream": true}'
    ```

=== "Response"

    `200`

    ```text
    event: complete
    data: {"bytes_downloaded": 0, "bytes_previously_downloaded": 0, "bytes_total": 0, "file": "llama-b11393-bin-ubuntu-vulkan-x64.tar.gz", "file_index": 1, "percent": 100, "total_download_size": 0, "total_files": 1}
    ```

=== "Schema"

    ```json
    {
      "description": "progress events report one file; the complete event follows the last one, and an error event replaces it on failure.",
      "type": "object",
      "properties": {
        "bytes_downloaded": {"type": "integer"},
        "bytes_previously_downloaded": {"type": "integer"},
        "bytes_total": {"type": "integer"},
        "code": {
          "description": "unknown_model when the model is not in the registry.",
          "type": "string"
        },
        "error": {"description": "Only on an error event.", "type": "string"},
        "file": {"type": "string"},
        "file_index": {"type": "integer"},
        "percent": {"type": "number"},
        "status": {
          "description": "Only on a complete event for an operation that had nothing to download.",
          "const": "ok"
        },
        "total_download_size": {"type": "integer"},
        "total_files": {"type": "integer"}
      }
    }
    ```
<!-- END GENERATED: lemonade.install -->

### Install or Uninstall a Local Backend

```bash
curl -X POST http://localhost:13305/v1/install \
  -H "Content-Type: application/json" \
  -d '{"recipe": "llamacpp", "backend": "vulkan"}'
```

```json
{"backend": "vulkan", "recipe": "llamacpp", "status": "success"}
```

```bash
curl -X POST http://localhost:13305/v1/uninstall \
  -H "Content-Type: application/json" \
  -d '{"recipe": "llamacpp", "backend": "vulkan"}'
```

```json
{"backend": "vulkan", "recipe": "llamacpp", "status": "success"}
```

### Install a Cloud Provider

Registers an OpenAI-compatible chat provider. The base URL is persisted to `config.json`; the optional `api_key` lives in `lemond` process memory only (cleared on restart). See the [Cloud Offload guide](../guide/configuration/cloud.md) for the full workflow.

The fields after `provider` and `base_url` are applied only when present in the request body. Re-installing a provider without them keeps its stored values, so updating just the `base_url` never resets a custom auth header or the `allow_insecure_http` opt-in.

`models_discovered` is `0` when no API key is resolvable. If `api_key` is supplied but the provider's environment variable is also set, the response includes a `warning` string explaining that the environment variable took precedence.

<!-- BEGIN GENERATED: lemonade.install_dry_run -->
## `POST /v1/install/dry-run`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Resolves the release asset [`POST /v1/install`](#post-v1install) would download for a recipe and backend, optionally for a GPU architecture this machine lacks, without downloading or installing anything.

Resolution uses the normal install-parameter machinery, so it may read local configuration and, for a backend pinned to `latest`, query GitHub release metadata. It neither downloads the asset nor checks that its URL exists. CI uses it to check architecture-to-asset resolution for hardware the runner lacks; `test/server_gfx_topology.py` checks the resulting URLs separately.

An architecture can resolve to a family target name the release repository uses: `gfx1201` resolves to `gfx120X`, as defined by `rocm_asset_families` in `backend_versions.json`. An `arch` outside Lemonade's support matrix still produces metadata, with `supported: false`.

Missing `recipe` or `backend` is answered with `400`. Invalid JSON, or a failure to resolve (an unknown recipe or backend, an unsupported platform, unavailable architecture detection, or a failed version lookup), is answered with `500` and an `error` string, plus `arch` when it was parsed.

Also served at `/api/v0/install/dry-run`, `/api/v1/install/dry-run` and `/v0/install/dry-run`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | Yes | Recipe name, e.g. `llamacpp`, `whispercpp` or `vllm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Backend within the recipe, e.g. `vulkan`, `rocm` or `rocm-nightly`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `arch` | No | ROCm GPU architecture to resolve for, e.g. `gfx1201`, overriding detection for this call. Omitted, the host is detected. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install/dry-run" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"recipe": "whispercpp", "backend": "rocm", "arch": "gfx1201"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/install/dry-run \
      -H "Content-Type: application/json" \
      -d '{"recipe": "whispercpp", "backend": "rocm", "arch": "gfx1201"}'
    ```

=== "Response"

    `200`

    ```json
    {
      "arch": "gfx1201",
      "backend": "rocm",
      "filename": "whisper-v1.8.4-linux-rocm-gfx120X.tar.gz",
      "recipe": "whispercpp",
      "repo": "lemonade-sdk/whisper.cpp-rocm",
      "supported": true,
      "supports_split_archive": false,
      "url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/download/v1.8.4/whisper-v1.8.4-linux-rocm-gfx120X.tar.gz",
      "version": "v1.8.4"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": [
        "recipe",
        "backend",
        "arch",
        "repo",
        "version",
        "filename",
        "url",
        "supports_split_archive",
        "supported"
      ],
      "properties": {
        "arch": {"description": "The requested arch, or \"\" when omitted.", "type": "string"},
        "backend": {"type": "string"},
        "filename": {"description": "Release asset name.", "type": "string"},
        "recipe": {"type": "string"},
        "repo": {"description": "GitHub repository of the release.", "type": "string"},
        "supported": {
          "description": "Whether Lemonade's support matrix accepts arch for this recipe and backend. Always true when arch is omitted. Not an asset existence check.",
          "type": "boolean"
        },
        "supports_split_archive": {
          "description": "Whether the recipe's assets may be published in parts, which the real download finds through a .partcount manifest.",
          "type": "boolean"
        },
        "url": {
          "description": "Release download URL built from repo, version and filename. Not checked.",
          "type": "string"
        },
        "version": {
          "description": "Release version: the pin in backend_versions.json unless a runtime version policy overrides it.",
          "type": "string"
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.install_dry_run -->

<!-- BEGIN GENERATED: lemonade.uninstall -->
## `POST /v1/uninstall`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Removes the backend for a recipe, or a cloud provider when `backend` is `"cloud"`.

**Status:** the cloud-provider branch is experimental.

Loaded models that use the backend are unloaded first.

Removing a cloud provider deletes its record from `config.json`, drops its in-memory API key, and evicts every model discovered for it. A provider that was never installed is answered with `404`.

A local uninstall without both `recipe` and `backend`, or a cloud uninstall without `provider`, is answered with `400`. Any other failure, including invalid JSON, is answered with `500` and an `error` string.

Also served at `/api/v0/uninstall`, `/api/v1/uninstall` and `/v0/uninstall`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: recipe name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Backend within the recipe, or `"cloud"` to remove a cloud provider. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, required: the installed provider's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/uninstall" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"backend": "cloud", "provider": "example"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/uninstall \
      -H "Content-Type: application/json" \
      -d '{"backend": "cloud", "provider": "example"}'
    ```

=== "Response"

    `200`

    ```json
    {"backend": "cloud", "models_evicted": 0, "provider": "example", "status": "success"}
    ```

=== "Schema"

    ```json
    {
      "oneOf": [
        {
          "description": "A local backend removed.",
          "type": "object",
          "required": ["status", "recipe", "backend"],
          "properties": {
            "backend": {"type": "string"},
            "recipe": {"type": "string"},
            "status": {"const": "success"}
          }
        },
        {
          "description": "A cloud provider removed.",
          "type": "object",
          "required": ["status", "backend", "provider", "models_evicted"],
          "properties": {
            "backend": {"const": "cloud"},
            "models_evicted": {
              "description": "Discovered models removed from the catalog.",
              "type": "integer"
            },
            "provider": {"type": "string"},
            "status": {"const": "success"}
          }
        }
      ]
    }
    ```
<!-- END GENERATED: lemonade.uninstall -->

<!-- BEGIN GENERATED: lemonade.cloud_auth -->
## `POST /v1/cloud/auth`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Stores an API key for an installed cloud provider in lemond's memory and refreshes the provider's discovered models.

The key is never written to disk and is cleared when lemond restarts; for a key that persists, set `LEMONADE_<PROVIDER>_API_KEY` in lemond's environment instead.

**Precedence:** when `LEMONADE_<PROVIDER>_API_KEY` is set, it wins: the supplied key is not stored and the answer is `409` with `{"error": {"type": "auth_conflict", "env_var": "LEMONADE_<PROVIDER>_API_KEY", "message": ...}}`. An operator can provision a house key this way without a client overriding it.

A missing or empty `provider` or `api_key` is answered with `400`, as is an `http://` provider without `allow_insecure_http` (code `insecure_http_requires_opt_in`). A provider that is not installed is answered with `404`; install it with [`POST /v1/install`](#post-v1install) and `backend: "cloud"` first.

Also served at `/api/v0/cloud/auth`, `/api/v1/cloud/auth` and `/v0/cloud/auth`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `provider` | Yes | Installed provider name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | Yes | API key to hold in lemond's memory. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Opt in to sending the key to a provider whose base URL is `http://`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/cloud/auth" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"provider": "example", "api_key": "example-key"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/cloud/auth \
      -H "Content-Type: application/json" \
      -d '{"provider": "example", "api_key": "example-key"}'
    ```

=== "Response"

    `200`

    ```json
    {
      "allow_insecure_http": false,
      "auth_state": {"env_var_set": false, "runtime_key_set": true},
      "models_discovered": 0,
      "provider": "example"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["provider", "allow_insecure_http", "auth_state", "models_discovered"],
      "properties": {
        "allow_insecure_http": {"type": "boolean"},
        "auth_state": {
          "type": "object",
          "properties": {"env_var_set": {"type": "boolean"}, "runtime_key_set": {"type": "boolean"}}
        },
        "models_discovered": {
          "description": "Chat models discovered with the key; 0 when discovery fails.",
          "type": "integer"
        },
        "provider": {"type": "string"},
        "warning": {
          "description": "The warnings joined into one string, for older clients.",
          "type": "string"
        },
        "warnings": {"type": "array", "items": {"type": "string"}}
      }
    }
    ```
<!-- END GENERATED: lemonade.cloud_auth -->

<!-- BEGIN GENERATED: lemonade.cloud_auth_provider -->
## `DELETE /v1/cloud/auth/{provider}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Clears the API key held in lemond's memory for a cloud provider. A key from `LEMONADE_<PROVIDER>_API_KEY` stays in effect.

Without an environment-variable key, the provider's discovered models are evicted from the catalog, since they can no longer authenticate.

Also served at `/api/v0/cloud/auth/{provider}`, `/api/v1/cloud/auth/{provider}` and `/v0/cloud/auth/{provider}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `provider` (path) | Yes | Installed provider name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/cloud/auth/example" `
      -Method DELETE
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/v1/cloud/auth/example
    ```

=== "Response"

    `200`

    ```json
    {
      "auth_state": {"env_var_set": false, "runtime_key_set": false},
      "cleared_runtime_key": true,
      "provider": "example"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["provider", "cleared_runtime_key", "auth_state"],
      "properties": {
        "auth_state": {
          "type": "object",
          "properties": {"env_var_set": {"type": "boolean"}, "runtime_key_set": {"type": "boolean"}}
        },
        "cleared_runtime_key": {
          "description": "false when no in-memory key was held, e.g. when the only key came from the environment variable.",
          "type": "boolean"
        },
        "provider": {"type": "string"}
      }
    }
    ```
<!-- END GENERATED: lemonade.cloud_auth_provider -->

<!-- BEGIN GENERATED: lemonade.live -->
## `GET /live`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lightweight liveness probe for load balancers and orchestrators. Unlike [`/v1/health`](#get-v1health), it does no work beyond confirming the process is up and does not inspect loaded models or backends, so it is safe to poll at high frequency.

`/live` is not versioned: it is not mounted under `/api/v0/`, `/api/v1/`, `/v0/` or `/v1/`. `HEAD /live` returns `200 OK` with an empty body.

Requires no API key.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/live"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/live
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
<!-- END GENERATED: lemonade.live -->

<!-- BEGIN GENERATED: lemonade.metrics -->
## `GET /metrics`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns Lemonade, model, backend and system metrics in the Prometheus text exposition format (`text/plain; version=0.0.4; charset=utf-8`), for Prometheus to scrape; Grafana queries Prometheus rather than this endpoint.

Unlike most Lemonade endpoints, `/metrics` is root-level only: it is not mounted under `/api/v0/`, `/api/v1/`, `/v0/` or `/v1/`. `HEAD /metrics` returns `200 OK` with an empty body.

**Authentication:** when `LEMONADE_API_KEY` is set, `/metrics` requires bearer authentication with either that key or `LEMONADE_ADMIN_API_KEY`. When only `LEMONADE_ADMIN_API_KEY` is set, `/metrics` needs no key, like the regular API endpoints.

**Metric families:** the names, types, labels and help text are defined by the `metrics.describe(...)` calls in [`src/cpp/server/prometheus_metrics.cpp`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/server/prometheus_metrics.cpp). Unsupported, unavailable, null, NaN and infinite values are omitted rather than emitted as samples.

**llama.cpp backend metrics:** Lemonade starts llama.cpp backends with metrics enabled and, for each loaded `llamacpp` model, makes a best-effort scrape of the backend's private `/metrics` endpoint. Those metrics are renamed under the `lemonade_llamacpp_*` prefix and labeled with the same model metadata as `lemonade_model_info`. A failed backend scrape does not fail the response.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Text`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/metrics"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/metrics
    ```

=== "Response"

    `200`

    ```text
    # HELP lemonade_server_up Whether the Lemonade server is running.
    # TYPE lemonade_server_up gauge
    lemonade_server_up 1
    # HELP lemonade_server_info Lemonade server build information.
    # TYPE lemonade_server_info gauge
    lemonade_server_info{version="2026.43.0~1686.1553ede6"} 1
    # HELP lemonade_loaded_models Number of models currently loaded in Lemonade.
    # TYPE lemonade_loaded_models gauge
    lemonade_loaded_models 4
    # HELP lemonade_model_info Metadata for each Lemonade model observed by this process.
    # TYPE lemonade_model_info gauge
    # HELP lemonade_model_loaded Whether this model is currently loaded in Lemonade.
    # TYPE lemonade_model_loaded gauge
    # HELP lemonade_model_input_tokens Latest input token count reported by a model.
    # TYPE lemonade_model_input_tokens gauge
    # HELP lemonade_model_output_tokens Latest output token count reported by a model.
    # TYPE lemonade_model_output_tokens gauge
    # HELP lemonade_model_prompt_tokens Latest prompt token count reported by a model.
    # TYPE lemonade_model_prompt_tokens gauge
    # HELP lemonade_model_cache_tokens Latest prompt tokens served from the backend prefix cache for a model.
    ...
    ```
<!-- END GENERATED: lemonade.metrics -->

### Polling and Refresh Rate

The `/metrics` endpoint has no internal refresh timer. It renders the latest server state at the moment it is scraped.

Polling frequency is configured in Prometheus via `scrape_interval`, for example:

```yaml
global:
  scrape_interval: 10s
```

Grafana queries Prometheus. Grafana's dashboard refresh controls how often panels query Prometheus, but it does not control how often Prometheus scrapes Lemonade.

<!-- BEGIN GENERATED: lemonade.health -->
## `GET /v1/health`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports that the server is up, its version, and the models it has loaded.

`HEAD` returns `200 OK` with an empty body.

Also served at `/api/v0/health`, `/api/v1/health` and `/v0/health`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/health"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/health
    ```

=== "Response"

    `200`

    ```json
    {
      "all_models_loaded": [
        {
          "backend_alive": true,
          "backend_health": "ready",
          "backend_url": "http://127.0.0.1:8001/v1",
          "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
          "device": "gpu",
          "is_busy": false,
          "is_streaming": false,
          "last_use": 680809329,
          "launch_command": ["~/.cache/lemonade/bin/llamacpp/vulkan/llama-server", "-m", "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "--ctx-size", "40960", "--port", "8001", "--jinja", ... 13 more],
          "loaded": true,
          "max_context_window": 40960,
          "model_name": "Qwen3-0.6B-GGUF",
          "pid": 2557435,
          "pinned": false,
          "recipe": "llamacpp",
          "recipe_options": {
            "ctx_size": 40960,
            "llamacpp_args": "--temp 0.6 --top-p 0.85 --top-k 20 --min-p 0.0 --repeat-penalty 1.0 --parallel 1"
          },
          "residency_class": "standard",
          "slot_pool": "standard/llm",
          "status": "ready",
          "type": "llm",
          "watchdog_reset": false
        }
      ],
      "max_models": {
        "classification": 1,
        "embedding": 1,
        "image": 1,
        "llm": 1,
        "reranking": 1,
        "transcription": 1,
        "tts": 1
      },
      "model_loaded": "Qwen3-0.6B-GGUF",
      "pinned_helper_models": {
        "classification": 0,
        "embedding": 0,
        "image": 0,
        "llm": 0,
        "reranking": 0,
        "transcription": 0,
        "tts": 0
      },
      "pinned_models": {
        "classification": 0,
        "embedding": 0,
        "image": 0,
        "llm": 0,
        "reranking": 0,
        "transcription": 0,
        "tts": 0
      },
      "status": "ok",
      "telemetry": {"enabled": false},
      "update_check_done": true,
      "version": "2026.43.0~1693.f279660b",
      "websocket_port": 9001
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": [
        "status",
        "version",
        "model_loaded",
        "all_models_loaded",
        "max_models",
        "pinned_models",
        "pinned_helper_models",
        "update_check_done"
      ],
      "properties": {
        "all_models_loaded": {
          "description": "Every loaded model whose backend is alive.",
          "type": "array",
          "items": {
            "type": "object",
            "required": [
              "model_name",
              "checkpoint",
              "type",
              "device",
              "recipe",
              "recipe_options",
              "pinned",
              "pid",
              "backend_url",
              "last_use"
            ],
            "properties": {
              "backend_alive": {"type": "boolean"},
              "backend_health": {"type": "string"},
              "backend_url": {
                "description": "URL of the backend process serving the model, for debugging.",
                "type": "string"
              },
              "checkpoint": {"type": "string"},
              "cost_input_per_million": {"type": "number"},
              "cost_output_per_million": {"type": "number"},
              "device": {
                "description": "Space-separated devices: cpu, gpu, npu, or a combination such as \"gpu npu\".",
                "type": "string"
              },
              "is_busy": {
                "description": "Whether the model has requests or maintenance in progress.",
                "type": "boolean"
              },
              "is_streaming": {
                "description": "Whether the model is generating output: true from a stream's first chunk until every stream completes.",
                "type": "boolean"
              },
              "last_use": {
                "description": "Time of the last load or inference, in milliseconds on the server's monotonic clock: compare values, do not read them as dates.",
                "type": "integer"
              },
              "launch_command": {
                "description": "The program and arguments that started the backend, with the values actually used: an automatic ctx_size appears as a number, and flags Lemonade added are included. Absent for cloud models, which start no program.",
                "type": "array",
                "items": {"type": "string"}
              },
              "loaded": {"type": "boolean"},
              "max_context_window": {"type": "integer"},
              "model_name": {"type": "string"},
              "pid": {"description": "Process id of the backend.", "type": "integer"},
              "pinned": {
                "description": "Whether the model is pinned against eviction.",
                "type": "boolean"
              },
              "recipe": {"type": "string"},
              "recipe_options": {
                "description": "Options the model was loaded with, such as ctx_size or llamacpp_backend.",
                "type": "object"
              },
              "residency_class": {
                "description": "standard, or routing_helper for a model a router policy keeps resident.",
                "type": "string"
              },
              "slot_pool": {
                "description": "The loaded-model limit the model counts against, or unmetered.",
                "type": "string"
              },
              "status": {"type": "string"},
              "type": {
                "description": "Model type, such as llm, embedding, reranking, transcription, image, tts or classification.",
                "type": "string"
              },
              "watchdog_reset": {
                "description": "Whether the backend watchdog has reset this backend.",
                "type": "boolean"
              },
              "watchdog_reset_reason": {"type": "string"}
            }
          }
        },
        "max_models": {
          "description": "Most models of each type that can be loaded at once, set by max_loaded_models.",
          "type": "object",
          "additionalProperties": {"type": "integer"}
        },
        "model_loaded": {
          "description": "The most recently used loaded model, or null.",
          "type": ["string", "null"]
        },
        "pinned_helper_models": {
          "description": "Pinned routing-helper models of each type.",
          "type": "object",
          "additionalProperties": {"type": "integer"}
        },
        "pinned_models": {
          "description": "Pinned loaded models of each type.",
          "type": "object",
          "additionalProperties": {"type": "integer"}
        },
        "status": {"const": "ok"},
        "telemetry": {
          "type": "object",
          "required": ["enabled"],
          "properties": {
            "captures": {
              "description": "Only when enabled: the request parts telemetry records.",
              "type": "array",
              "items": {"enum": ["inputs", "outputs", "thinking"]}
            },
            "enabled": {"description": "Whether telemetry collection is active.", "type": "boolean"}
          }
        },
        "update_check_done": {
          "description": "Whether the startup model update check has finished; update_available fields are ready once it has.",
          "type": "boolean"
        },
        "version": {"description": "Lemonade Server version.", "type": "string"},
        "websocket_port": {
          "description": "Only while the WebSocket server runs: the dedicated port of the Realtime and Log Streaming APIs, OS-assigned or set by --websocket-port. The main port serves both APIs too.",
          "type": "integer"
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.health -->

<!-- BEGIN GENERATED: lemonade.docs -->
## `GET /v1/docs`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the API reference pages bundled with the server. The pages ship with the server, so they describe the version actually running and need no internet access.

Fetch this index first, then read the pages it advertises with [`GET /v1/docs/{page}`](#get-v1docspage). Every entry carries its own `url`, so new pages can appear in future releases without breaking clients.

Also served at `/api/v0/docs`, `/api/v1/docs` and `/v0/docs`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/docs"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/docs
    ```

=== "Response"

    `200`

    ```json
    {
      "docs": [
        {
          "bytes": 1778,
          "id": "api/README",
          "title": "Lemonade Endpoints Spec",
          "url": "/v1/docs/api/README"
        },
        {
          "bytes": 893,
          "id": "api/anthropic",
          "title": "Anthropic-Compatible API",
          "url": "/v1/docs/api/anthropic"
        },
        {
          "bytes": 453,
          "id": "api/internal",
          "title": "Internal API",
          "url": "/v1/docs/api/internal"
        },
        {
          "bytes": 99280,
          "id": "api/lemonade",
          "title": "Lemonade API",
          "url": "/v1/docs/api/lemonade"
        },
        {
          "bytes": 14841,
          "id": "api/llamacpp",
          "title": "llama.cpp-Specific API",
          "url": "/v1/docs/api/llamacpp"
        },
        {"bytes": 18109, "id": "api/mcp", "title": "MCP Gateway", "url": "/v1/docs/api/mcp"},
        {
          "bytes": 1180,
          "id": "api/ollama",
          "title": "Ollama-Compatible API",
          "url": "/v1/docs/api/ollama"
        },
        {
          "bytes": 55451,
          "id": "api/openai",
          "title": "OpenAI-Compatible API",
          "url": "/v1/docs/api/openai"
        },
        {"bytes": 2967, "id": "api/router", "title": "Router API", "url": "/v1/docs/api/router"}
      ],
      "format": "text/markdown",
      "version": "2026.43.0~1686.1553ede6"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["version", "format", "docs"],
      "properties": {
        "docs": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["id", "title", "url", "bytes"],
            "properties": {
              "bytes": {"type": "integer"},
              "id": {
                "description": "Page id, mirroring the page's path on the documentation website.",
                "type": "string"
              },
              "title": {"type": "string"},
              "url": {
                "description": "Where to read the page, under the same prefix the index was requested with: a client that queries /api/v0/docs receives /api/v0/docs/... URLs.",
                "type": "string"
              }
            }
          }
        },
        "format": {"const": "text/markdown"},
        "version": {"description": "Lemonade Server version the pages describe.", "type": "string"}
      }
    }
    ```
<!-- END GENERATED: lemonade.docs -->

<!-- BEGIN GENERATED: lemonade.docs_page -->
## `GET /v1/docs/{page}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns one bundled page as Markdown (`Content-Type: text/markdown`).

Unknown pages return `404`.

Also served at `/api/v0/docs/{page}`, `/api/v1/docs/{page}` and `/v0/docs/{page}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `page` (path) | Yes | The `id` from the [`GET /v1/docs`](#get-v1docs) index, which mirrors the page's path on the documentation website. The `.md` suffix is optional. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Text`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/docs/api/README"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/docs/api/README
    ```

=== "Response"

    `200`

    ```text
    # Lemonade Endpoints Spec

    The Lemonade HTTP service provides a wide array of standards-compliant and custom endpoints.

    Our design philosophy is:

    1. Ensure that Lemonade works out-of-box with all popular local AI apps.
    2. Prioritize using a pre-existing standard for functionality whenever possible.
    3. Add sufficient custom functionality to enable developers to build highly polished experiences.

    This spec details all supported endpoints. It is organized into pages that correspond to  which organization (OpenAI, Ollama, Lemonade, etc.) defined the endpoints.

    | API | Description |
    |-----|-------------|
    | [OpenAI-Compatible API](./openai.md) | Start here for the main API surface used by most SDKs and clients. |
    | [Ollama-Compatible API](./ollama.md) | Use this if your client expects Ollama-style behavior and routes. |
    | [Anthropic-Compatible API](./anthropic.md) | Use this for clients built around Anthropic's message format. |
    | [MCP Gateway](./mcp.md) | Use this to expose Lemonade as a Model Context Protocol server (POST /mcp). |
    | [llama.cpp-Specific API](./llamacpp.md) | Reference for llama.cpp-specific compatibility and conventions. |
    | [Lemonade-Specific API](./lemonade.md) | Local-first API for managing lifecycle, configuration, backends, etc. |
    ...
    ```
<!-- END GENERATED: lemonade.docs_page -->

### Reading the Files Directly

The same files are installed on disk, so they can be read without a running server:

| Platform | Path |
|----------|------|
| Windows (per-user) | `%LOCALAPPDATA%\lemonade_server\bin\resources\docs\` |
| Windows (all users) | `C:\Program Files\Lemonade Server\bin\resources\docs\` |
| macOS | `/Library/Application Support/Lemonade/resources/docs/` |
| Linux (local) | `/usr/local/share/lemonade-server/resources/docs/` |
| Linux (system) | `/usr/share/lemonade-server/resources/docs/` |
| Linux (optional prefix) | `/opt/share/lemonade-server/resources/docs/` |
| Linux (per-user) | `~/.local/share/lemonade-server/resources/docs/` |

<!-- BEGIN GENERATED: lemonade.stats -->
## `GET /v1/stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports performance statistics from the last inference request, plus counters accumulated since the server started.

`HEAD` returns `200 OK` with an empty body.

Also served at `/api/v0/stats`, `/api/v1/stats` and `/v0/stats`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/stats"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/stats
    ```

=== "Response"

    `200`

    ```json
    {
      "cache_tokens": 22,
      "cache_tokens_total": 44,
      "input_tokens": 1,
      "input_tokens_total": 50,
      "output_tokens": 10,
      "output_tokens_total": 302,
      "prompt_tokens": 23,
      "prompt_tokens_total": 94,
      "request_count_total": 5,
      "routing_decisions_total": 0,
      "routing_switches_total": 0,
      "time_to_first_token": 0.003305,
      "tokens_per_second": 351.87864096649326
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "properties": {
        "cache_tokens": {
          "description": "Prompt tokens served from the backend's prefix cache: llama.cpp's timings.cache_n, or usage.prompt_tokens_details.cached_tokens (input_tokens_details.cached_tokens for the Responses API) from OpenAI-compatible cloud providers. null when the last request reported no cache usage.",
          "type": ["integer", "null"]
        },
        "cache_tokens_total": {"type": "integer"},
        "input_tokens": {"description": "Tokens processed.", "type": ["integer", "null"]},
        "input_tokens_total": {"type": "integer"},
        "output_tokens": {"description": "Tokens generated.", "type": ["integer", "null"]},
        "output_tokens_total": {"type": "integer"},
        "prompt_tokens": {
          "description": "Prompt tokens, including cached tokens.",
          "type": ["integer", "null"]
        },
        "prompt_tokens_total": {"type": "integer"},
        "request_count_total": {
          "description": "Requests since the server started. The other *_total fields also count since start.",
          "type": "integer"
        },
        "routing_decisions_total": {
          "description": "Routing decisions made by collection.router dispatch.",
          "type": "integer"
        },
        "routing_switches_total": {
          "description": "Routing decisions that changed a conversation's routed model, a proxy for route ping-pong. A conversation is identified by a hash of its system prompt and first user message.",
          "type": "integer"
        },
        "time_to_first_token": {
          "description": "Seconds until the first token was generated.",
          "type": ["number", "null"]
        },
        "tokens_per_second": {"description": "Generation speed.", "type": ["number", "null"]}
      }
    }
    ```
<!-- END GENERATED: lemonade.stats -->

<!-- BEGIN GENERATED: lemonade.system_stats -->
## `GET /v1/system-stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports current host resource usage as measured by the server process, for first-party clients and dashboards that want lightweight telemetry without scraping Prometheus.

GPU, VRAM and NPU readings depend on the operating system and installed drivers; an unavailable reading is `null`. `HEAD` returns `200 OK` with an empty body.

Also served at `/api/v0/system-stats`, `/api/v1/system-stats` and `/v0/system-stats`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/system-stats"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/system-stats
    ```

=== "Response"

    `200`

    ```json
    {
      "cpu_percent": 7.5416776925112465,
      "gpu_percent": 10.0,
      "memory_gb": 56.4,
      "npu_percent": 0.0,
      "vram_gb": 33.63154220581055
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["cpu_percent", "memory_gb", "gpu_percent", "vram_gb", "npu_percent"],
      "properties": {
        "cpu_percent": {
          "description": "System CPU utilization since the previous reading.",
          "type": ["number", "null"]
        },
        "gpu_percent": {"description": "GPU utilization.", "type": ["number", "null"]},
        "memory_gb": {"description": "System RAM in use, in GiB.", "type": "number"},
        "npu_percent": {"description": "NPU utilization.", "type": ["number", "null"]},
        "vram_gb": {"description": "GPU memory in use, in GiB.", "type": ["number", "null"]}
      }
    }
    ```
<!-- END GENERATED: lemonade.system_stats -->

<!-- BEGIN GENERATED: lemonade.system_info -->
## `GET /v1/system-info`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports hardware details, detected devices, each recipe's backend support on this system, model storage, and installed cloud providers.

`HEAD` returns `200 OK` with an empty body.

Also served at `/api/v0/system-info`, `/api/v1/system-info` and `/v0/system-info`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/system-info"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/system-info
    ```

=== "Response"

    `200`

    ```json
    {
      "OS Version": "Linux-7.0.0-34-generic (Ubuntu 26.04)",
      "Physical Memory": "122.69 GB",
      "Processor": "AMD RYZEN AI MAX+ 395 w/ Radeon 8060S",
      "cloud": {"providers": []},
      "devices": {
        "amd_gpu": [
          {
            "available": true,
            "family": "gfx1151",
            "integrated": true,
            "name": "Radeon 8060S Graphics (gfx1151)",
            "virtual_mem_gb": 61.3,
            "vram_gb": 0.5
          }
        ],
        "amd_npu": {
          "available": true,
          "family": "XDNA2",
          "name": "AMD RYZEN AI MAX+ 395 w/ Radeon 8060S",
          "power_mode": "DEFAULT",
          "tops_max_int": 58,
          "utilization": 0.0
        },
        "cpu": {
          "available": true,
          "cores": 16,
          "family": "x86_64",
          "name": "AMD RYZEN AI MAX+ 395 w/ Radeon 8060S",
          "threads": 32
        },
        "nvidia_gpu": [{"available": false, "error": "No NVIDIA discrete GPU found", "name": ""}]
      },
      "model_storage": {
        "free_bytes": 623403810816,
        "path": "~/.cache/huggingface/hub",
        "total_bytes": 2013755310080,
        "used_bytes": 1390351499264
      },
      "no_fetch_executables": false,
      "recipes": {
        "acestep": {
          "backends": {
            "cuda": {
              "action": "",
              "devices": [],
              "download_filename": "acestep-cuda-linux-x64.tar.gz",
              "message": "Unsupported GPU",
              "release_url": "https://github.com/pwilkin/acestep.cpp/releases/tag/v0.1.1",
              "state": "unsupported",
              "version": "v0.1.1"
            },
            "rocm": {
              "action": "lemonade backends install acestep:rocm",
              "devices": ["amd_gpu"],
              "download_filename": "acestep-rocm-linux-x64.tar.gz",
              "message": "Backend is supported but not installed.",
              "release_url": "https://github.com/pwilkin/acestep.cpp/releases/tag/v0.1.1",
              "state": "installable",
              "version": "v0.1.1"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "acestep-vulkan-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/pwilkin/acestep.cpp/releases/tag/v0.1.1",
              "state": "installed",
              "version": "v0.1.1"
            }
          },
          "default_backend": "vulkan",
          "display_name": "ACE-Step",
          "experimental": true,
          "modality": "Audio generation",
          "options": [
            {
              "cli_flag": "--acestep",
              "default": "",
              "group": "Audio Generation Options",
              "help": "ACE-Step backend to use",
              "name": "acestep_backend",
              "type_name": "BACKEND"
            }
          ],
          "order": 13,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "cuda",
              "device_summary": "NVIDIA GPUs",
              "devices": [{"device": "nvidia_gpu", "families": []}],
              "os": ["linux", "windows"]
            },
            {
              "backend": "vulkan",
              "device_summary": "Vulkan-capable GPUs",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["x86_64"]},
                {"device": "nvidia_gpu", "families": []}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "rocm",
              "device_summary": "Supported AMD ROCm iGPU/dGPU families (ROCm via TheRock)",
              "devices": [
                {
                  "device": "amd_gpu",
                  "families": ["gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X"]
                }
              ],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "ACE-Step"
        },
        "ds4": {
          "backends": {
            "rocm": {
              "action": "lemonade backends install ds4:rocm",
              "devices": ["amd_gpu"],
              "download_filename": "ds4-b0001-linux-rocm-gfx1151-x64.tar.gz",
              "message": "Backend is supported but not installed.",
              "release_url": "https://github.com/lemonade-sdk/ds4-rocm/releases/tag/b0001",
              "state": "installable",
              "version": "b0001"
            }
          },
          "default_backend": "rocm",
          "display_name": "DwarfStar4 (experimental)",
          "experimental": true,
          "modality": "Text generation",
          "options": [
            {
              "cli_flag": "--ds4-args",
              "default": "",
              "group": "DS4 Options",
              "help": "Custom arguments to pass to ds4-server",
              "name": "ds4_args",
              "type_name": "ARGS"
            }
          ],
          "order": 10,
          "selectable_backend": false,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "rocm",
              "device_summary": "Prebuilt ds4 for AMD Strix Halo",
              "devices": [{"device": "amd_gpu", "families": ["gfx1151"]}],
              "os": ["linux"]
            }
          ],
          "uses_ctx_size": true,
          "web_display_name": "DwarfStar4 (experimental)"
        },
        "flm": {
          "backends": {
            "npu": {
              "action": "lemonade backends install flm:npu",
              "devices": ["amd_npu"],
              "download_filename": "fastflowlm_1.0.7_linux.tar.gz",
              "message": "Backend update is required before use.",
              "release_url": "https://github.com/ROCm/FastFlowLM/releases/tag/v1.0.7",
              "state": "update_required",
              "version": "v1.0.5"
            }
          },
          "default_backend": "npu",
          "display_name": "FastFlowLM NPU",
          "experimental": false,
          "modality": "Text generation",
          "options": [
            {
              "cli_flag": "--flm-args",
              "default": "",
              "group": "FastFlowLM Options",
              "help": "Safe flm serve tuning args: --pmode, --prefill-chunk-len, --img-pre-resize, --socket, --q-len, --preemption",
              "name": "flm_args",
              "type_name": "ARGS"
            }
          ],
          "order": 6,
          "selectable_backend": false,
          "slot_policy": "coexist_by_type",
          "support": [
            {
              "backend": "npu",
              "device_summary": "XDNA2 NPU",
              "devices": [{"device": "amd_npu", "families": ["XDNA2"]}],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": true,
          "web_display_name": "FastFlowLM NPU"
        },
        "kokoro": {
          "backends": {
            "cpu": {
              "action": "",
              "devices": ["cpu"],
              "download_filename": "kokoros-linux-x86_64.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/Kokoros/releases/tag/b21",
              "state": "installed",
              "version": "b21"
            },
            "metal": {
              "action": "",
              "can_uninstall": true,
              "devices": [],
              "message": "Requires macOS",
              "release_url": "https://github.com/lemonade-sdk/Kokoros/releases/tag/b21",
              "state": "unsupported",
              "version": "b21"
            }
          },
          "default_backend": "cpu",
          "display_name": "Kokoro",
          "experimental": false,
          "modality": "Text-to-speech",
          "options": [],
          "order": 4,
          "selectable_backend": false,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "metal",
              "device_summary": "Apple Silicon GPU",
              "devices": [{"device": "metal", "families": []}],
              "os": ["macos"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64 CPU",
              "devices": [{"device": "cpu", "families": ["x86_64"]}],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "Kokoro"
        },
        "llamacpp": {
          "backends": {
            "cpu": {
              "action": "lemonade backends install llamacpp:cpu",
              "devices": ["cpu"],
              "download_filename": "llama-b11393-bin-ubuntu-x64.tar.gz",
              "message": "Backend is supported but not installed.",
              "release_url": "https://github.com/ggml-org/llama.cpp/releases/tag/b11393",
              "state": "installable",
              "version": "b11393"
            },
            "cuda": {
              "action": "",
              "devices": [],
              "message": "Unsupported GPU",
              "state": "unsupported"
            },
            "metal": {
              "action": "",
              "can_uninstall": true,
              "devices": [],
              "message": "Requires macOS",
              "state": "unsupported"
            },
            "rocm": {
              "action": "",
              "devices": ["amd_gpu"],
              "download_filename": "llama-b11395-bin-ubuntu-rocm-10.0-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/llama.cpp/releases/tag/b11395",
              "state": "installed",
              "version": "b11395"
            },
            "system": {
              "action": "",
              "devices": ["cpu"],
              "message": "llama-server not found in PATH",
              "release_url": "https://github.com//releases/tag/",
              "state": "unsupported"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "llama-b11393-bin-ubuntu-vulkan-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/ggml-org/llama.cpp/releases/tag/b11393",
              "state": "installed",
              "version": "b11393"
            }
          },
          "default_backend": "vulkan",
          "display_name": "Llama.cpp GPU",
          "experimental": false,
          "modality": "Text generation",
          "options": [
            {
              "cli_flag": "--llamacpp",
              "default": "",
              "group": "Llama.cpp Backend Options",
              "help": "LlamaCpp backend to use",
              "name": "llamacpp_backend",
              "type_name": "BACKEND"
            },
            {
              "cli_flag": "--llamacpp-device",
              "default": "",
              "group": "Llama.cpp Backend Options",
              "help": "Comma-separated list of accelerator devices to use (e.g. Vulkan0)",
              "name": "llamacpp_device",
              "type_name": "DEVICES"
            },
            {
              "cli_flag": "--llamacpp-args",
              "default": "",
              "group": "Llama.cpp Backend Options",
              "help": "Custom arguments to pass to llama-server",
              "name": "llamacpp_args",
              "type_name": "ARGS"
            }
          ],
          "order": 0,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "system",
              "device_summary": "x86_64/ARM64 CPU, GPU",
              "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
              "os": ["linux"]
            },
            {
              "backend": "metal",
              "device_summary": "Apple Silicon GPU",
              "devices": [{"device": "metal", "families": []}],
              "os": ["macos"]
            },
            {
              "backend": "cuda",
              "device_summary": "NVIDIA GPUs (Turing or newer)**",
              "devices": [
                {
                  "device": "nvidia_gpu",
                  "families": [
                    "sm_100",
                    "sm_120",
                    "sm_121",
                    "sm_75",
                    "sm_80",
                    "sm_86",
                    "sm_89",
                    "sm_90"
                  ]
                }
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "vulkan",
              "device_summary": "x86_64 CPU, AMD iGPU, AMD dGPU; ARM64 CPU/GPU (Linux)",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["arm64", "x86_64"]}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "rocm",
              "device_summary": "AMD GPUs supported by ROCm",
              "devices": [
                {
                  "device": "amd_gpu",
                  "families": [
                    "gfx103X",
                    "gfx110X",
                    "gfx1150",
                    "gfx1151",
                    "gfx1152",
                    "gfx120X",
                    "gfx908",
                    "gfx90a",
                    "gfx942",
                    "gfx950"
                  ]
                }
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64 CPU; ARM64 CPU (Linux)",
              "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": true,
          "web_display_name": "llama.cpp GPU"
        },
        "llamacpp-hrx": {
          "backends": {
            "hrx": {
              "action": "lemonade backends install llamacpp-hrx:hrx",
              "devices": ["amd_gpu"],
              "download_filename": "llama-hrx-b99-bin-manylinux-hrx-x64.tar.gz",
              "message": "Backend update is required before use.",
              "release_url": "https://github.com/ROCm/ggml-staging-automation/releases/tag/hrx-b99",
              "state": "update_required",
              "version": "hrx-b59"
            }
          },
          "default_backend": "hrx",
          "display_name": "HRX GPU (experimental)",
          "experimental": true,
          "modality": "Text generation",
          "options": [
            {
              "cli_flag": "--hrx-args",
              "default": "",
              "group": "HRX Options",
              "help": "Custom arguments to pass to the HRX llama-server",
              "name": "hrx_args",
              "type_name": "ARGS"
            }
          ],
          "order": 1,
          "selectable_backend": false,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "hrx",
              "device_summary": "AMD GPUs (gfx1100, gfx1151)",
              "devices": [{"device": "amd_gpu", "families": ["gfx1100", "gfx1151"]}],
              "os": ["linux"]
            }
          ],
          "uses_ctx_size": true,
          "web_display_name": "HRX GPU (experimental)"
        },
        "moonshine": {
          "backends": {
            "cpu": {
              "action": "",
              "devices": ["cpu"],
              "download_filename": "moonshine-server-moonshine0.0.62-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/moonshine-server-rocm/releases/tag/moonshine0.0.62",
              "state": "installed",
              "version": "moonshine0.0.62"
            }
          },
          "default_backend": "cpu",
          "display_name": "Moonshine",
          "experimental": false,
          "modality": "Speech-to-text",
          "options": [
            {
              "cli_flag": "--moonshine-args",
              "default": "",
              "group": "Moonshine Options",
              "help": "Custom arguments to pass to moonshine-server",
              "name": "moonshine_args",
              "type_name": "ARGS"
            }
          ],
          "order": 3,
          "selectable_backend": false,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "cpu",
              "device_summary": "x86_64/arm64 CPU",
              "devices": [{"device": "cpu", "families": ["x86_64"]}],
              "os": ["windows"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64/arm64 CPU",
              "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
              "os": ["linux"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64/arm64 CPU",
              "devices": [{"device": "cpu", "families": ["arm64"]}],
              "os": ["macos"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "Moonshine"
        },
        "onnxruntime": {
          "backends": {
            "cpu": {
              "action": "",
              "devices": ["cpu"],
              "download_filename": "ort-server-0.3.7-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/ort-server/releases/tag/0.3.7",
              "state": "installed",
              "version": "0.3.7"
            }
          },
          "default_backend": "cpu",
          "display_name": "ONNX Runtime",
          "experimental": true,
          "modality": "Text classification",
          "options": [
            {
              "cli_flag": "--onnxruntime-args",
              "default": "",
              "group": "ONNX Runtime Options",
              "help": "Custom arguments to pass to ort-server",
              "name": "onnxruntime_args",
              "type_name": "ARGS"
            }
          ],
          "order": 14,
          "selectable_backend": false,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "cpu",
              "device_summary": "x86_64 CPU",
              "devices": [{"device": "cpu", "families": ["x86_64"]}],
              "os": ["windows"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64/arm64 CPU",
              "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
              "os": ["linux"]
            },
            {
              "backend": "cpu",
              "device_summary": "arm64 CPU",
              "devices": [{"device": "cpu", "families": ["arm64"]}],
              "os": ["macos"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "ONNX Runtime"
        },
        "openmoss": {
          "backends": {
            "cuda": {
              "action": "",
              "devices": [],
              "download_filename": "moss-tts-cuda-linux-x64.tar.gz",
              "message": "Unsupported GPU",
              "release_url": "https://github.com/pwilkin/openmoss/releases/tag/v0.3.0",
              "state": "unsupported",
              "version": "v0.3.0"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "moss-tts-vulkan-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/pwilkin/openmoss/releases/tag/v0.3.0",
              "state": "installed",
              "version": "v0.3.0"
            }
          },
          "default_backend": "vulkan",
          "display_name": "OpenMOSS TTS",
          "experimental": true,
          "modality": "Text-to-speech",
          "options": [
            {
              "cli_flag": "--openmoss",
              "default": "",
              "group": "Text-to-Speech Options",
              "help": "OpenMOSS TTS backend to use",
              "name": "openmoss_backend",
              "type_name": "BACKEND"
            }
          ],
          "order": 16,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "cuda",
              "device_summary": "NVIDIA GPUs",
              "devices": [{"device": "nvidia_gpu", "families": []}],
              "os": ["linux", "windows"]
            },
            {
              "backend": "vulkan",
              "device_summary": "Vulkan-capable GPUs",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["x86_64"]},
                {"device": "nvidia_gpu", "families": []}
              ],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "OpenMOSS TTS"
        },
        "ryzenai-llm": {
          "backends": {
            "npu": {
              "action": "",
              "can_uninstall": true,
              "devices": [],
              "download_filename": "ryzenai-server.zip",
              "message": "Requires Windows",
              "release_url": "https://github.com/lemonade-sdk/ryzenai-server/releases/tag/v1.7.0",
              "state": "unsupported",
              "version": "v1.7.0"
            }
          },
          "display_name": "Ryzen AI LLM",
          "experimental": false,
          "modality": "Text generation",
          "options": [],
          "order": 7,
          "selectable_backend": false,
          "slot_policy": "exclusive_npu",
          "support": [
            {
              "backend": "npu",
              "device_summary": "XDNA2 NPU",
              "devices": [{"device": "amd_npu", "families": ["XDNA2"]}],
              "os": ["windows"]
            }
          ],
          "uses_ctx_size": true,
          "web_display_name": "Ryzen AI SW NPU"
        },
        "sd-cpp": {
          "backends": {
            "cpu": {
              "action": "",
              "devices": ["cpu"],
              "download_filename": "sd-master-017cc8e-bin-Linux-Ubuntu-24.04-x86_64.zip",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
              "state": "installed",
              "version": "master-894-017cc8e"
            },
            "cuda": {
              "action": "",
              "devices": [],
              "message": "Unsupported GPU",
              "state": "unsupported"
            },
            "metal": {
              "action": "",
              "can_uninstall": true,
              "devices": [],
              "message": "Requires macOS",
              "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
              "state": "unsupported",
              "version": "master-894-017cc8e"
            },
            "rocm": {
              "action": "lemonade backends install sd-cpp:rocm",
              "devices": ["amd_gpu"],
              "download_filename": "sd-master-017cc8e-bin-Linux-Ubuntu-24.04-x86_64-rocm-10.0.0.zip",
              "message": "Backend update is required before use.",
              "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
              "state": "update_required",
              "version": "master-843-462d675"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "sd-master-017cc8e-bin-Linux-Ubuntu-24.04-x86_64-vulkan.zip",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
              "state": "installed",
              "version": "master-894-017cc8e"
            }
          },
          "default_backend": "vulkan",
          "display_name": "StableDiffusion.cpp",
          "experimental": false,
          "modality": "Image generation",
          "options": [
            {
              "cli_flag": "--sdcpp",
              "default": "",
              "group": "Stable Diffusion Options",
              "help": "SD.cpp backend to use",
              "name": "sd-cpp_backend",
              "type_name": "BACKEND"
            },
            {
              "cli_flag": "--sdcpp-args",
              "default": "",
              "group": "Stable Diffusion Options",
              "help": "Custom arguments to pass to sd-server (must not conflict with managed args)",
              "name": "sdcpp_args",
              "type_name": "ARGS"
            },
            {
              "cli_flag": "",
              "default": 20,
              "group": "Stable Diffusion Options",
              "help": "Number of diffusion steps",
              "name": "steps",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 7.0,
              "group": "Stable Diffusion Options",
              "help": "Classifier-free guidance scale",
              "name": "cfg_scale",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 512,
              "group": "Stable Diffusion Options",
              "help": "Output image width",
              "name": "width",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 512,
              "group": "Stable Diffusion Options",
              "help": "Output image height",
              "name": "height",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": "",
              "group": "Stable Diffusion Options",
              "help": "Sampling method",
              "name": "sampling_method",
              "type_name": "ARGS"
            },
            {
              "cli_flag": "",
              "default": 0.0,
              "group": "Stable Diffusion Options",
              "help": "Flow shift",
              "name": "flow_shift",
              "type_name": "SIZE"
            }
          ],
          "order": 5,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "metal",
              "device_summary": "Apple Silicon GPU",
              "devices": [{"device": "metal", "families": []}],
              "os": ["macos"]
            },
            {
              "backend": "cuda",
              "device_summary": "NVIDIA GPUs (Turing or newer)**",
              "devices": [
                {
                  "device": "nvidia_gpu",
                  "families": [
                    "sm_100",
                    "sm_120",
                    "sm_121",
                    "sm_75",
                    "sm_80",
                    "sm_86",
                    "sm_89",
                    "sm_90"
                  ]
                }
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "vulkan",
              "device_summary": "Vulkan-capable GPUs",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["x86_64"]},
                {"device": "nvidia_gpu", "families": []}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "rocm",
              "device_summary": "Supported AMD ROCm iGPU/dGPU families*",
              "devices": [
                {
                  "device": "amd_gpu",
                  "families": ["gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X"]
                }
              ],
              "os": ["linux"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64 CPU",
              "devices": [{"device": "cpu", "families": ["x86_64"]}],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "stable-diffusion.cpp"
        },
        "thenoise": {
          "backends": {
            "rocm": {
              "action": "lemonade backends install thenoise:rocm",
              "devices": ["amd_gpu"],
              "download_filename": "thenoise-0.10.0-rocm10.1.0-gfx1151-x64.tar.gz",
              "message": "Backend update is required before use.",
              "release_url": "https://github.com/lemonade-sdk/thenoise/releases/tag/thenoise-0.10.0-rocm10.1.0",
              "state": "update_required",
              "version": "thenoise-0.4.1-rocm7.14.0"
            }
          },
          "default_backend": "rocm",
          "display_name": "TheNoise ROCm",
          "experimental": true,
          "modality": "Image generation",
          "options": [
            {
              "cli_flag": "--thenoise",
              "default": "",
              "group": "TheNoise Options",
              "help": "TheNoise backend to use",
              "name": "thenoise_backend",
              "type_name": "BACKEND"
            },
            {
              "cli_flag": "",
              "default": 20,
              "group": "TheNoise Options",
              "help": "Number of denoising steps",
              "name": "steps",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 7.0,
              "group": "TheNoise Options",
              "help": "CFG scale (<= 1.0 disables CFG)",
              "name": "cfg_scale",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 512,
              "group": "TheNoise Options",
              "help": "Output image width",
              "name": "width",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 512,
              "group": "TheNoise Options",
              "help": "Output image height",
              "name": "height",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": "",
              "group": "TheNoise Options",
              "help": "Denoising solver (euler | er_sde)",
              "name": "sampler",
              "type_name": "ARGS"
            },
            {
              "cli_flag": "",
              "default": "",
              "group": "TheNoise Options",
              "help": "Negative prompt",
              "name": "negative_prompt",
              "type_name": "ARGS"
            },
            {
              "cli_flag": "",
              "default": false,
              "group": "TheNoise Options",
              "help": "Nyquist notch post-filter (removes 2px grid artifacts)",
              "name": "qwen_vae_enhance",
              "type_name": "BOOL"
            },
            {
              "cli_flag": "",
              "default": 0.0,
              "group": "TheNoise Options",
              "help": "Film grain strength (0.0-10.0)",
              "name": "film_grain",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": 0.0,
              "group": "TheNoise Options",
              "help": "RCAS sharpening strength (0.0-1.0)",
              "name": "sharpening",
              "type_name": "SIZE"
            },
            {
              "cli_flag": "",
              "default": "",
              "group": "TheNoise Options",
              "help": "Comma-separated LoRA specs, e.g. \"style:0.8,sub/detail:0.5\"",
              "name": "lora_specs",
              "type_name": "ARGS"
            }
          ],
          "order": 9,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "rocm",
              "device_summary": "Supported AMD ROCm families",
              "devices": [
                {
                  "device": "amd_gpu",
                  "families": ["gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X"]
                }
              ],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "thenoise"
        },
        "thinksound": {
          "backends": {
            "cuda": {
              "action": "",
              "devices": [],
              "download_filename": "thinksound-cuda-linux-x64.tar.gz",
              "message": "Unsupported GPU",
              "release_url": "https://github.com/pwilkin/thinksound.cpp/releases/tag/v0.1.2",
              "state": "unsupported",
              "version": "v0.1.2"
            },
            "rocm": {
              "action": "lemonade backends install thinksound:rocm",
              "devices": ["amd_gpu"],
              "download_filename": "thinksound-rocm-linux-x64.tar.gz",
              "message": "Backend is supported but not installed.",
              "release_url": "https://github.com/pwilkin/thinksound.cpp/releases/tag/v0.1.2",
              "state": "installable",
              "version": "v0.1.2"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "thinksound-vulkan-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/pwilkin/thinksound.cpp/releases/tag/v0.1.2",
              "state": "installed",
              "version": "v0.1.2"
            }
          },
          "default_backend": "vulkan",
          "display_name": "ThinkSound",
          "experimental": true,
          "modality": "Audio generation",
          "options": [
            {
              "cli_flag": "--thinksound",
              "default": "",
              "group": "Audio Generation Options",
              "help": "ThinkSound backend to use",
              "name": "thinksound_backend",
              "type_name": "BACKEND"
            }
          ],
          "order": 12,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "cuda",
              "device_summary": "NVIDIA GPUs",
              "devices": [{"device": "nvidia_gpu", "families": []}],
              "os": ["linux", "windows"]
            },
            {
              "backend": "vulkan",
              "device_summary": "Vulkan-capable GPUs",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["x86_64"]},
                {"device": "nvidia_gpu", "families": []}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "rocm",
              "device_summary": "Supported AMD ROCm iGPU/dGPU families (ROCm via TheRock)",
              "devices": [
                {
                  "device": "amd_gpu",
                  "families": ["gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X"]
                }
              ],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "ThinkSound"
        },
        "trellis": {
          "backends": {
            "cuda": {
              "action": "",
              "devices": [],
              "download_filename": "trellis-cuda-linux-x64.tar.gz",
              "message": "Unsupported GPU",
              "release_url": "https://github.com/pwilkin/trellis.cpp/releases/tag/v0.4.3",
              "state": "unsupported",
              "version": "v0.4.3"
            },
            "rocm": {
              "action": "",
              "devices": ["amd_gpu"],
              "download_filename": "trellis-rocm-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/pwilkin/trellis.cpp/releases/tag/v0.4.3",
              "state": "installed",
              "version": "v0.4.3"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "trellis-vulkan-linux-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/pwilkin/trellis.cpp/releases/tag/v0.4.3",
              "state": "installed",
              "version": "v0.4.3"
            }
          },
          "default_backend": "vulkan",
          "display_name": "TRELLIS.2",
          "experimental": true,
          "modality": "3D generation",
          "options": [
            {
              "cli_flag": "--trellis",
              "default": "",
              "group": "3D Generation Options",
              "help": "Trellis backend to use",
              "name": "trellis_backend",
              "type_name": "BACKEND"
            },
            {
              "cli_flag": "--trellis-args",
              "default": "",
              "group": "3D Generation Options",
              "help": "Custom arguments to pass to trellis-server",
              "name": "trellis_args",
              "type_name": "ARGS"
            }
          ],
          "order": 15,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "cuda",
              "device_summary": "NVIDIA GPUs",
              "devices": [{"device": "nvidia_gpu", "families": []}],
              "os": ["linux", "windows"]
            },
            {
              "backend": "vulkan",
              "device_summary": "Vulkan-capable GPUs",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["x86_64"]},
                {"device": "nvidia_gpu", "families": []}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "rocm",
              "device_summary": "Supported AMD ROCm iGPU/dGPU families (ROCm via TheRock)",
              "devices": [
                {
                  "device": "amd_gpu",
                  "families": ["gfx103X", "gfx110X", "gfx1150", "gfx1151", "gfx1152", "gfx120X"]
                }
              ],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "TRELLIS.2"
        },
        "vllm": {
          "backends": {
            "rocm": {
              "action": "",
              "devices": ["amd_gpu"],
              "download_filename": "vllm0.20.1-rocm7.12.0-gfx1151-x64.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/vllm-rocm/releases/tag/vllm0.20.1-rocm7.12.0-gfx1151",
              "state": "installed",
              "version": "vllm0.20.1-rocm7.12.0-gfx1151"
            }
          },
          "default_backend": "rocm",
          "display_name": "vLLM ROCm (experimental)",
          "experimental": true,
          "modality": "Text generation",
          "options": [
            {
              "cli_flag": "--vllm",
              "default": "",
              "group": "vLLM Options",
              "help": "vLLM backend to use",
              "name": "vllm_backend",
              "type_name": "BACKEND"
            },
            {
              "cli_flag": "--vllm-args",
              "default": "",
              "group": "vLLM Options",
              "help": "Custom arguments to pass to vllm-server",
              "name": "vllm_args",
              "type_name": "ARGS"
            }
          ],
          "order": 8,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "rocm",
              "device_summary": "Strix Halo iGPU (gfx1151)",
              "devices": [
                {"device": "amd_gpu", "families": ["gfx110X", "gfx1150", "gfx1151", "gfx120X"]}
              ],
              "os": ["linux"]
            }
          ],
          "uses_ctx_size": true,
          "web_display_name": "vLLM ROCm (experimental)"
        },
        "whispercpp": {
          "backends": {
            "cpu": {
              "action": "lemonade backends install whispercpp:cpu",
              "devices": ["cpu"],
              "download_filename": "whisper-v1.8.4-linux-cpu-x86_64.tar.gz",
              "message": "Backend is supported but not installed.",
              "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
              "state": "installable",
              "version": "v1.8.4"
            },
            "metal": {
              "action": "",
              "can_uninstall": true,
              "devices": [],
              "download_filename": "whisper-v1.8.4-darwin-metal-arm64.tar.gz",
              "message": "Requires macOS",
              "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
              "state": "unsupported",
              "version": "v1.8.4"
            },
            "npu": {
              "action": "",
              "can_uninstall": true,
              "devices": [],
              "message": "Requires Windows",
              "state": "unsupported"
            },
            "rocm": {
              "action": "",
              "devices": ["amd_gpu"],
              "download_filename": "whisper-v1.8.4-linux-rocm-gfx1151.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
              "state": "installed",
              "version": "v1.8.4"
            },
            "vulkan": {
              "action": "",
              "devices": ["amd_gpu", "cpu"],
              "download_filename": "whisper-v1.8.4-linux-vulkan-x86_64.tar.gz",
              "message": "",
              "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
              "state": "installed",
              "version": "v1.8.4"
            }
          },
          "default_backend": "vulkan",
          "display_name": "Whisper.cpp",
          "experimental": false,
          "modality": "Speech-to-text",
          "options": [
            {
              "cli_flag": "--whispercpp",
              "default": "",
              "group": "Whisper.cpp Options",
              "help": "WhisperCpp backend to use",
              "name": "whispercpp_backend",
              "type_name": "BACKEND"
            },
            {
              "cli_flag": "--whispercpp-args",
              "default": "",
              "group": "Whisper.cpp Options",
              "help": "Custom arguments to pass to whisper-server",
              "name": "whispercpp_args",
              "type_name": "ARGS"
            }
          ],
          "order": 2,
          "selectable_backend": true,
          "slot_policy": "standard",
          "support": [
            {
              "backend": "npu",
              "device_summary": "XDNA2 NPU",
              "devices": [{"device": "amd_npu", "families": ["XDNA2"]}],
              "os": ["windows"]
            },
            {
              "backend": "metal",
              "device_summary": "Apple Silicon GPU",
              "devices": [{"device": "metal", "families": []}],
              "os": ["macos"]
            },
            {
              "backend": "vulkan",
              "device_summary": "x86_64 CPU",
              "devices": [
                {"device": "amd_gpu", "families": []},
                {"device": "cpu", "families": ["x86_64"]}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "rocm",
              "device_summary": "Supported AMD ROCm iGPU/dGPU families*",
              "devices": [
                {"device": "amd_gpu", "families": ["gfx110X", "gfx1150", "gfx1151", "gfx120X"]}
              ],
              "os": ["linux", "windows"]
            },
            {
              "backend": "cpu",
              "device_summary": "x86_64 CPU",
              "devices": [{"device": "cpu", "families": ["x86_64"]}],
              "os": ["linux", "windows"]
            }
          ],
          "uses_ctx_size": false,
          "web_display_name": "whisper.cpp"
        }
      },
      "unavailable_recipes": []
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["devices", "recipes", "unavailable_recipes", "model_storage", "cloud"],
      "properties": {
        "BIOS Version": {"description": "Windows only.", "type": "string"},
        "CPU Max Clock": {"description": "Windows only.", "type": "string"},
        "OEM System": {"description": "Windows only: system or laptop model.", "type": "string"},
        "OS Version": {"description": "Operating system name and version.", "type": "string"},
        "Physical Memory": {"description": "Total RAM.", "type": "string"},
        "Processor": {"description": "CPU model name.", "type": "string"},
        "Windows Power Setting": {
          "description": "Windows only: current power plan.",
          "type": "string"
        },
        "cloud": {
          "type": "object",
          "required": ["providers"],
          "properties": {
            "providers": {
              "description": "One entry per installed cloud provider; the API key itself is never reported.",
              "type": "array",
              "items": {
                "type": "object",
                "properties": {
                  "allow_insecure_http": {"type": "boolean"},
                  "auth_header_name": {
                    "description": "Header the API key is sent in; default Authorization.",
                    "type": "string"
                  },
                  "auth_header_prefix": {
                    "description": "Value prefix before the key; default \"Bearer \".",
                    "type": "string"
                  },
                  "base_url": {
                    "description": "Base URL persisted in config.json.",
                    "type": "string"
                  },
                  "env_var": {
                    "description": "Name of the provider's API key environment variable, e.g. LEMONADE_FIREWORKS_API_KEY.",
                    "type": "string"
                  },
                  "env_var_set": {
                    "description": "Whether that variable is set in lemond's environment.",
                    "type": "boolean"
                  },
                  "models_discovered": {
                    "description": "Chat-capable models in the catalog for this provider.",
                    "type": "integer"
                  },
                  "name": {
                    "description": "Provider name, used as the model-name prefix, e.g. fireworks.",
                    "type": "string"
                  },
                  "runtime_key_set": {
                    "description": "Whether a key was supplied with POST /v1/cloud/auth since the server started.",
                    "type": "boolean"
                  },
                  "warning": {"type": "string"},
                  "warnings": {"type": "array", "items": {"type": "string"}},
                  "wire_format": {"enum": ["openai", "anthropic"]}
                }
              }
            }
          }
        },
        "devices": {
          "description": "Hardware detected on the system, without software support information: cpu, plus amd_gpu and nvidia_gpu arrays and amd_npu when present.",
          "type": "object",
          "properties": {
            "amd_gpu": {"description": "AMD GPUs, integrated and discrete.", "type": "array"},
            "amd_npu": {"type": "object"},
            "cpu": {"description": "Name, cores and threads.", "type": "object"},
            "nvidia_gpu": {"type": "array"}
          }
        },
        "model_storage": {
          "description": "Drive-level storage for the configured model storage path, for storage meters. Not a recursive sum of model files.",
          "type": "object",
          "required": ["path", "used_bytes", "total_bytes", "free_bytes"],
          "properties": {
            "error": {"description": "Only when the drive could not be measured.", "type": "string"},
            "free_bytes": {
              "description": "Bytes available to the server process.",
              "type": ["integer", "null"]
            },
            "path": {"type": "string"},
            "total_bytes": {"type": ["integer", "null"]},
            "used_bytes": {"type": ["integer", "null"]}
          }
        },
        "no_fetch_executables": {
          "description": "Whether the server is configured not to download backend executables.",
          "type": "boolean"
        },
        "recipes": {
          "description": "Each recipe's backends and their support on this system.",
          "type": "object",
          "additionalProperties": {
            "type": "object",
            "required": ["backends"],
            "properties": {
              "backends": {
                "type": "object",
                "additionalProperties": {
                  "type": "object",
                  "required": ["devices", "state"],
                  "properties": {
                    "action": {
                      "description": "What the user should do, typically an exact CLI command for install and update, or a URL; may be empty.",
                      "type": "string"
                    },
                    "devices": {
                      "description": "Devices on this system that support the backend; empty when unsupported.",
                      "type": "array",
                      "items": {"type": "string"}
                    },
                    "download_filename": {"type": "string"},
                    "message": {
                      "description": "Status text for GUI and CLI users: required for unsupported, installable and update_required; empty for installed.",
                      "type": "string"
                    },
                    "release_url": {"type": "string"},
                    "state": {
                      "enum": ["unsupported", "installable", "update_required", "installed"]
                    },
                    "version": {
                      "description": "Installed or configured backend version, when available.",
                      "type": "string"
                    }
                  }
                }
              },
              "default_backend": {
                "description": "Backend the server prefers on this system; present when at least one backend is not unsupported.",
                "type": "string"
              }
            }
          }
        },
        "unavailable_recipes": {
          "description": "Recipes all of whose models are filtered out on this host. Reported separately so recipes stays the same on every host; dynamic-model backends (cloud, flm) are never listed.",
          "type": "array",
          "items": {"type": "string"}
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.system_info -->

## Log Streaming API (WebSocket)
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Stream server logs over WebSocket. Clients connect, send a subscribe message, and receive a snapshot of recent log history followed by live log entries as they occur.

### Connection

Connect on the main HTTP port, the same way as the [Realtime Audio Transcription API](./openai.md#ws-realtime). The separate `websocket_port` that [`/v1/health`](#get-v1health) reports is a legacy listener:

```
ws://localhost:13305/logs/stream
```

After connecting, send a `logs.subscribe` message to start receiving logs.

### Client to Server Messages

| Message Type | Description |
|--------------|-------------|
| `logs.subscribe` | Subscribe to log stream. Optional `after_seq` field to resume from a specific sequence number. |

### Server to Client Messages

| Message Type | Description |
|--------------|-------------|
| `logs.snapshot` | Initial batch of retained log entries (up to 5000). Sent once after subscribing. |
| `logs.entry` | A single live log entry. Sent as new log lines are emitted. |
| `error` | Error message (e.g., invalid subscribe request). |

### Example: Subscribe to Logs

Subscribe from the beginning (full backlog):

```json
{
  "type": "logs.subscribe",
  "after_seq": null
}
```

Resume after a known sequence number (e.g., on reconnect):

```json
{
  "type": "logs.subscribe",
  "after_seq": 1042
}
```

### Example: Snapshot Response

```json
{
  "type": "logs.snapshot",
  "entries": [
    {
      "seq": 1,
      "timestamp": "2025-03-30 14:22:01.123",
      "severity": "Info",
      "tag": "Server",
      "line": "2025-03-30 14:22:01.123 [Info] (Server) Starting Lemonade Server..."
    }
  ]
}
```

### Example: Live Entry

```json
{
  "type": "logs.entry",
  "entry": {
    "seq": 1043,
    "timestamp": "2025-03-30 14:22:05.456",
    "severity": "Info",
    "tag": "Router",
    "line": "2025-03-30 14:22:05.456 [Info] (Router) Model loaded successfully"
  }
}
```

### Log Entry Fields

| Field | Type | Description |
|-------|------|-------------|
| `seq` | integer | Monotonically increasing sequence number. Use for dedup and resume. |
| `timestamp` | string | Formatted timestamp from the log system. |
| `severity` | string | Log level: `Trace`, `Debug`, `Info`, `Warning`, `Error`, `Fatal`. |
| `tag` | string | Log source tag (e.g., `Server`, `Router`, component name). |
| `line` | string | The full formatted log line. |

### Integration Notes

- **Reconnection**: Track the last `seq` received and pass it as `after_seq` on reconnect to avoid duplicate entries.
- **Backlog**: The server retains up to 5000 recent log entries. The snapshot may be smaller if fewer entries exist.
- **Platform availability**: WebSocket log streaming is available on all platforms (Windows, Linux, and macOS).

## Job Engine

<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Run client-posted sequences of server operations as durable, background **jobs**: steps that pass data forward, branch on results, and have a pause, interrupt, resume, delete and query lifecycle that survives client disconnect and server restart. Exclusive ops (`load`/`unload`/`chat`) hold a Router slot so normal traffic queues behind a running job.

See [`docs/dev/job-system.md`](../dev/job-system.md) for the step schema, op set, and lifecycle, and [`docs/dev/job-expression-language.md`](../dev/job-expression-language.md) for the `when`/`branch` expression grammar.

<!-- BEGIN GENERATED: lemonade.jobs_create -->
## `POST /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Creates a job: a sequence of server operations that runs in the background, passes data between steps and branches on results, surviving client disconnects and server restarts. See [Job Engine](#job-engine).

The step graph is validated at creation; an invalid one is answered with `400`. When the job store holds 50 jobs that are all still active or resumable, the answer is `429` until one is deleted or finishes.

Also served at `/api/v0/jobs`, `/api/v1/jobs` and `/v0/jobs`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `name` | No | Display name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | The steps, in order; see [Recipe: steps](../dev/job-system.md#recipe-steps). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `definition` | No | Alternative to `steps`: an object whose `steps` holds the steps. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `inputs` | No | Values the steps read as `context.inputs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/jobs" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"name": "example", "steps": [{"id": "wait", "op": "sleep", "params": {"ms": 30000}}]}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs \
      -H "Content-Type: application/json" \
      -d '{"name": "example", "steps": [{"id": "wait", "op": "sleep", "params": {"ms": 30000}}]}'
    ```

=== "Response"

    `202`

    ```json
    {"id": "job-20261007-171503-000001"}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id"],
      "properties": {"id": {"description": "The job's id.", "type": "string"}}
    }
    ```
<!-- END GENERATED: lemonade.jobs_create -->

<!-- BEGIN GENERATED: lemonade.jobs_list -->
## `GET /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Lists a summary of every job, active or finished.

Also served at `/api/v0/jobs`, `/api/v1/jobs` and `/v0/jobs`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/jobs"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs
    ```

=== "Response"

    `200`

    ```json
    {
      "jobs": [
        {
          "id": "job-20261007-171503-000002",
          "name": "example",
          "status": "queued",
          "created_at": "2026-10-07T17:15:03Z",
          "progress": {"cursor": "wait", "completed": 0, "step_count": 1}
        },
        {
          "id": "job-20261007-171503-000001",
          "name": "example",
          "status": "running",
          "created_at": "2026-10-07T17:15:03Z",
          "progress": {"cursor": "wait", "completed": 0, "step_count": 1}
        }
      ]
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["jobs"],
      "properties": {
        "jobs": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["id", "name", "status", "created_at", "progress"],
            "properties": {
              "created_at": {"type": "string"},
              "error": {"type": "string"},
              "finished_at": {"type": "string"},
              "id": {"type": "string"},
              "name": {"type": "string"},
              "progress": {
                "type": "object",
                "properties": {
                  "completed": {
                    "description": "Steps that need no more work: completed, skipped, or failed with the failure handled.",
                    "type": "integer"
                  },
                  "cursor": {"description": "Id of the current or next step.", "type": "string"},
                  "step_count": {"type": "integer"}
                }
              },
              "status": {
                "enum": ["queued", "running", "paused", "interrupted", "completed", "failed"]
              },
              "summary": {"type": "string"}
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: lemonade.jobs_list -->

<!-- BEGIN GENERATED: lemonade.jobs_pause -->
## `POST /v1/jobs/{id}/pause`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Stops a job after its current step and releases the model slot, so queued requests run. A queued job pauses at once. Models the job loaded stay loaded for the resume.

An unknown job, or one that is not queued or running, is answered with `404`.

Also served at `/api/v0/jobs/{id}/pause`, `/api/v1/jobs/{id}/pause` and `/v0/jobs/{id}/pause`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000003/pause" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/jobs/job-20261007-171503-000003/pause
    ```

=== "Response"

    `200`

    ```json
    {"status": "pausing"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "pausing"}}}
    ```
<!-- END GENERATED: lemonade.jobs_pause -->

<!-- BEGIN GENERATED: lemonade.jobs_interrupt -->
## `POST /v1/jobs/{id}/interrupt`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Cancels a job's current step now, aborting an in-flight load or chat. The step returns to pending, so resuming re-runs it, and the models the job loaded are unloaded.

An unknown job, or one that is not queued or running, is answered with `404`.

Also served at `/api/v0/jobs/{id}/interrupt`, `/api/v1/jobs/{id}/interrupt` and `/v0/jobs/{id}/interrupt`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000004/interrupt" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/jobs/job-20261007-171503-000004/interrupt
    ```

=== "Response"

    `200`

    ```json
    {"status": "interrupting"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "interrupting"}}}
    ```
<!-- END GENERATED: lemonade.jobs_interrupt -->

<!-- BEGIN GENERATED: lemonade.jobs_resume -->
## `POST /v1/jobs/{id}/resume`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Continues a paused job at its next step, or re-runs an interrupted job's pending step after reloading the models the interrupt unloaded.

An unknown job, or one that is not paused or interrupted, is answered with `404`.

Also served at `/api/v0/jobs/{id}/resume`, `/api/v1/jobs/{id}/resume` and `/v0/jobs/{id}/resume`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000006/resume" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/jobs/job-20261007-171503-000006/resume
    ```

=== "Response"

    `200`

    ```json
    {"status": "resuming"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "resuming"}}}
    ```
<!-- END GENERATED: lemonade.jobs_resume -->

<!-- BEGIN GENERATED: lemonade.jobs_get -->
## `GET /v1/jobs/{id}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Returns a job's full record: its status, the state of each step, and the context the steps have written.

An unknown job is answered with `404`.

Also served at `/api/v0/jobs/{id}`, `/api/v1/jobs/{id}` and `/v0/jobs/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000007"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs/job-20261007-171503-000007
    ```

=== "Response"

    `200`

    ```json
    {
      "id": "job-20261007-171503-000007",
      "name": "example",
      "status": "queued",
      "inputs": {},
      "context": {"inputs": {}},
      "steps": [
        {
          "id": "wait",
          "op": "sleep",
          "params": {"ms": 30000},
          "status": "pending",
          "duration_ms": 0
        }
      ],
      "cursor": "wait",
      "created_at": "2026-10-07T17:15:03Z"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id", "name", "status", "inputs", "context", "steps", "cursor", "created_at"],
      "properties": {
        "context": {
          "description": "Each completed step's output under its id, the extracted keys, and inputs.",
          "type": "object"
        },
        "created_at": {"type": "string"},
        "cursor": {"description": "Id of the current or next step.", "type": "string"},
        "error": {"type": "string"},
        "finished_at": {"type": "string"},
        "id": {"type": "string"},
        "inputs": {"type": "object"},
        "name": {"type": "string"},
        "started_at": {"type": "string"},
        "status": {"enum": ["queued", "running", "paused", "interrupted", "completed", "failed"]},
        "steps": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["id", "op", "params", "status", "duration_ms"],
            "properties": {
              "duration_ms": {"type": "integer"},
              "error": {"type": "string"},
              "id": {"type": "string"},
              "op": {"type": "string"},
              "output": {},
              "params": {"type": "object"},
              "status": {"enum": ["pending", "running", "completed", "failed", "skipped"]}
            }
          }
        },
        "summary": {"type": "string"}
      }
    }
    ```
<!-- END GENERATED: lemonade.jobs_get -->

<!-- BEGIN GENERATED: lemonade.jobs_delete -->
## `DELETE /v1/jobs/{id}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Removes a job. An active job is interrupted first, and the models it loaded are unloaded before it disappears.

An unknown job is answered with `404`.

Also served at `/api/v0/jobs/{id}`, `/api/v1/jobs/{id}` and `/v0/jobs/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000008" `
      -Method DELETE
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/v1/jobs/job-20261007-171503-000008
    ```

=== "Response"

    `200`

    ```json
    {"status": "deleted"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "deleted"}}}
    ```
<!-- END GENERATED: lemonade.jobs_delete -->
