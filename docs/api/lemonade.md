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
| `WS` | [`/logs/stream`](#log-streaming-api-websocket) | Log Streaming |
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

Some Lemonade endpoints live on other pages: the per-model [`/v1/models/{id}/files`](openai.md#get-v1modelsidfiles) and [`/v1/models/{id}/options`](openai.md#get-v1modelsidoptions) extensions sit with the OpenAI models endpoints, routing and [`/v1/routing/validate`](router.md#post-v1routingvalidate) are on the [Router API](router.md) page, and endpoints for clients bundled with the server are on the [Internal API](internal.md) page.

<!-- BEGIN GENERATED: lemonade.models_check_updates -->
## `POST /v1/models/check-updates`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly checks downloaded Hugging Face-backed models for newer upstream
commits. This is the manual counterpart to the startup update check and works
even when `auto_check_model_updates=false`.

Full offline mode remains authoritative: when `offline=true`, this endpoint
returns HTTP 409 and does not make network requests.

Also served at `/api/v0/models/check-updates`, `/api/v1/models/check-updates` and `/v0/models/check-updates`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

The same action is available from the CLI:

```bash
lemonade check-updates
```

### Example request

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

### Response format

`200`

```json
{"status": "success", "updates_available": 0, "models": [], "failed_models": {}}
```

**Field Descriptions:**

- `status` - failed when any model's check failed.
- `models` - Models with a newer upstream commit.
- `failed_models` - Each model whose check failed, with its error.
<!-- END GENERATED: lemonade.models_check_updates -->

<!-- BEGIN GENERATED: lemonade.models_register -->
## `POST /v1/models/register`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Register or update a `user.*` model definition without downloading model files.
Use this endpoint when registration and installation are separate actions.
`POST /v1/pull` remains the install/download path and performs the same internal
registration step before downloading.

Also served at `/api/v0/models/register`, `/api/v1/models/register` and `/v0/models/register`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | Non-empty namespaced model name under `user.*`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | Yes | Lemonade recipe associated with the model definition. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | Main checkpoint, when the recipe uses one. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Named checkpoints for multi-checkpoint models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry or local source. Remote values are `huggingface` or `modelscope`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Additional model labels. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Already-registered component model names for collection recipes. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A checkpoint is intentionally not universally required: registration is a model
metadata operation and some present or future model types may not have local
weights. `/pull` remains the operation that attempts installation/download.

The endpoint accepts one model definition. An embedded `models` array represents
multiple definitions and remains a collection-import concern; register those
component definitions first when using this endpoint.

A `400` answers a body that is not a JSON object, a missing `model_name`, a name outside the `user.` namespace or with a reserved `extra.`/`builtin.` prefix, a missing `recipe`, and malformed `checkpoint`, `checkpoints`, `source` or `components` values.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/models/register" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model_name": "user.Phi-4-Mini-GGUF",
          "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M",
          "recipe": "llamacpp"
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/models/register \
      -H "Content-Type: application/json" \
      -d '{
          "model_name": "user.Phi-4-Mini-GGUF",
          "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M",
          "recipe": "llamacpp"
        }'
    ```

### Response format

`200`

```json
{
  "status": "success",
  "model_name": "Phi-4-Mini-GGUF",
  "canonical_model_name": "user.Phi-4-Mini-GGUF",
  "model": {
    "id": "Phi-4-Mini-GGUF",
    "created": 1234567890,
    "object": "model",
    "owned_by": "lemonade",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M",
    "recipe": "llamacpp",
    "downloaded": false,
    "update_available": false,
    "suggested": true,
    "labels": ["chat", "custom"],
    "components": [],
    "checkpoints": {"main": "unsloth/Phi-4-mini-instruct-GGUF:Q3_K_M"},
    "source": "huggingface",
    "registry_source": "huggingface",
    "recipe_options": {}
  }
}
```

**Field Descriptions:**

- `model`
  - `id` - Model identifier (used for loading and inference requests)
  - `created` - Unix timestamp of when the model entry was created
  - `object` - Type of object, always `"model"`
  - `owned_by` - Owner of the model, always `"lemonade"`
  - `checkpoint` - Full checkpoint identifier on Hugging Face
  - `recipe` - Backend/device recipe used to load the model (e.g., `"ryzenai-llm"`, `"llamacpp"`, `"flm"`)
  - `size` - Model size in GB (omitted for models without size information)
  - `max_context_window` - Optional integer indicating the maximum model-supported text context discovered from local static metadata. Currently populated for downloaded GGUF/llama.cpp models and installed FLM text-context models.
  - `context_length` - Number of tokens the model can handle in one request. Uses the loaded value when the model is running, the configured `ctx_size`, or the discovered cloud provider context length otherwise (omitted when neither is known).
  - `max_output_tokens` - Optional integer indicating the maximum output tokens the model can generate in a single completion, when reported by cloud providers.
  - `max_completion_tokens` - Optional integer alias for `max_output_tokens` adhering to OpenAI convention, when reported by cloud providers.
  - `downloaded` - Boolean indicating if the model is downloaded and available locally
  - `update_available` - Boolean indicating a newer commit exists on HuggingFace for this model. Only set for downloaded HF-backed models. `false` otherwise.
  - `suggested` - Boolean indicating if the model is recommended for general use
  - `labels` - Array of tags describing the model's capabilities and characteristics. See [Model Labels](./openai.md#model-labels) for the full list.
  - `image_defaults` - (Image models only) Default generation parameters for the model:
    - `steps` - Number of inference steps (e.g., 4 for turbo models, 20 for standard models)
    - `cfg_scale` - Classifier-free guidance scale (e.g., 1.0 for turbo models, 7.5 for standard models)
    - `width` - Default image width in pixels
    - `height` - Default image height in pixels
    - `soft_pixel_cap` - Largest output area, in pixels, that images/edits infers from the first reference image when size is omitted.
  - `components` - (Omni collections only, `recipe: "collection.omni"`) Ordered array of the component model names that make up the collection
  - `models` - (Omni collections only) Ordered array embedding each component's full model object (same shape as the entries in this list), parallel to `components`. This makes a collection's `/v1/models/{id}` response self-contained — exporting it produces a file that can be imported elsewhere via [`/v1/pull`](./lemonade.md#post-v1pull)
  - `checkpoints` - Every checkpoint by role, such as main, mmproj, draft, text_encoder or vae.
  - `source` - Where the model came from: a registry name, local_upload, local_path or extra_models_dir.
  - `registry_source` - Remote registry the checkpoint downloads from: huggingface or modelscope.
  - `recipe_options` - Options saved for this model in recipe_options.json.
  - `cloud_provider` - Cloud models only: the provider that serves the model.
  - `cost_input_per_million` - Cloud models only: USD per million input tokens, when the provider reports it.
  - `cost_output_per_million` - Cloud models only: USD per million output tokens, when the provider reports it.
  - `system_prompt` - Omni collections only: the collection's system prompt override.
  - `audio_defaults` - Audio models only: default generation parameters.
  - `version` - Router collections only: the policy document's version.
  - `routing` - Router collections only: the routing policy.
<!-- END GENERATED: lemonade.models_register -->

`model_name` is the public ID exposed by `/v1/models`; `canonical_model_name` is
the stable `user.*` registration ID. Registration updates `user_models.json` and
invalidates the model cache, but does not start a model download.

<!-- BEGIN GENERATED: lemonade.pull -->
## `POST /v1/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Register and install models for use with Lemonade Server.

The Lemonade Server built-in model registry has a collection of model names that can be pulled and loaded. The `pull` endpoint can install any registered model, and it can also register-then-install any model available on Hugging Face.

Also served at `/api/v0/pull`, `/api/v1/pull` and `/v0/pull`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `stream` | No | If `true`, returns Server-Sent Events (SSE) with download progress. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Only applies when `stream=true`. If `false`, the server starts a background model download job and returns a JSON snapshot immediately instead of keeping the HTTP response subscribed to SSE progress. Defaults to `true` for backwards compatibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` | Yes | Already registered: [Lemonade Server model name](https://lemonade-server.ai/models.html) to install. Register and install: Namespaced [Lemonade Server model name](https://lemonade-server.ai/models.html) to register and install. Omni-model: Namespaced model name, e.g. `user.MyKit`. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | No | Register and install, required: Lemonade API recipe to load the model with. Omni-model, required: Must be `"collection.omni"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | HuggingFace "main" checkpoint to install. A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | HuggingFace checkpoints to install, for multi-checkpoint models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reasoning` | No | Whether the model is a reasoning model, like DeepSeek (default: false). Adds 'reasoning' label. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vision` | No | Whether the model has vision capabilities for processing images (default: false). Adds 'vision' label. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `embedding` | No | Whether the model is an embedding model (default: false). Adds 'embeddings' label. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reranking` | No | Whether the model is a reranking model (default: false). Adds 'reranking' label. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mmproj` | No | Multimodal Projector (mmproj) file to use for vision models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Omni-model, required: Ordered, non-empty array of model names. Each component must be a regular model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `models` | No | Ordered array of full model definitions, one per `components` entry (the same fields as single-model registration, keyed by `model_name`). When present, component names that are not yet registered are registered from these definitions; names that already exist keep their local definition. When absent, every `components` entry must already exist in the registry (built-in or a previously registered `user.*` model). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Registering only: model labels. A model deploys in exactly one [deployment mode](./openai.md#model-labels): naming a mode the recipe cannot serve, or naming two, is rejected with `400`. Omitting the deployment label applies the recipe's default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry to download from: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `registry_source` | No | Same as `source`; when both are given they must name the same registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `do_not_upgrade` | No | `true` skips checking an already-downloaded model's registry for updates. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `local_import` | No | Register model files a client already copied into the Hugging Face cache, without downloading. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

**Install a Model that is Already Registered**

Example request:

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF"
  }'
```

Response format:

```json
{
  "status":"success",
  "model_name":"Qwen3-0.6B-GGUF"
}
```

In case of an error, the `error` field will contain the error message.

**Register and Install a Model**

Registration will place an entry for that model in the `user_models.json` file, which is located in the user's Lemonade config directory (default: `~/.config/lemonade`). Then, the model will be installed. Once the model is registered and installed, it will show up in the `models` endpoint alongside the built-in models and can be loaded.

The `recipe` field defines which software framework and device will be used to load and run the model.

> Note: the `model_name` for registering a new model must use the `user` namespace, to prevent collisions with built-in models. For example, `user.Phi-4-Mini-GGUF`.

A model definition requires at least a `main` checkpoint. This can be either
be specified with the `checkpoint` parameter, or a `main` key in the
`checkpoints` dict.

Each backend serves a fixed set of [deployment modes](openai.md#model-labels),
and a model deploys in exactly one of them. Naming a mode the recipe cannot
serve, or naming two — whether through `labels` or through the `embedding` /
`reranking` parameters — is rejected with `400` and nothing is registered:

```bash
curl -X POST http://localhost:13305/api/v1/pull \
  -H "Content-Type: application/json" \
  -d '{"model_name": "user.Clf", "recipe": "llamacpp",
       "checkpoint": "example/model:Q4_K_M", "labels": ["classification"]}'
```

```json
{"error": "Model 'user.Clf': recipe 'llamacpp' cannot serve 'classification'. It serves 'chat', 'embeddings', 'reranking'. Omit the label to deploy as 'chat'."}
```

```bash
curl -X POST http://localhost:13305/api/v1/pull \
  -H "Content-Type: application/json" \
  -d '{"model_name": "user.Both", "recipe": "llamacpp",
       "checkpoint": "example/model:Q4_K_M", "labels": ["chat", "embeddings"]}'
```

```json
{"error": "Model 'user.Both': a model deploys in exactly one mode, but these labels name two: 'chat' and 'embeddings'. Register one model per mode."}
```

Omitting the deployment label entirely is always valid — the recipe's default is
applied.

Other checkpoint types may also be specified depending on the model type.
This list is not exhaustive, and may change or grow over time as models
and backends evolve:
* `mmproj` - used by vision models, if not already embedded in `main`
* `draft` - used by dflash, eagle, and multitoken-prediction, if not already embedded in `main`
* `text_encoder` - text-to-token encoder used by image generation
* `vae` - variational autoencoder used by image generation

Example request:

```bash
# Single checkpoint
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.Phi-4-Mini-GGUF",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M",
    "recipe": "llamacpp"
  }'
```

Instead of defining a model by `checkpoint` and `mmproj`, a model can also
be defined with a dict of checkpoint types and paths. These requests do the
same thing, but the syntax for pulling the mmproj differs.

```bash
# Multi-checkpoint
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
# Multi-checkpoint
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

Response format:

```json
{
  "status":"success",
  "model_name":"user.Phi-4-Mini-GGUF"
}
```

In case of an error, the `error` field will contain the error message.

**Register an Omni-Model**

An omni collection is a collection type that bundles several models into a single entry that can be loaded, pulled, or deleted as a unit. Use `recipe: "collection.omni"` with a `components` array instead of `checkpoint`.

Components do not need to be downloaded already — any not-yet-downloaded components are pulled by the same call. Deleting the collection removes only the collection entry; components stay on disk.

Example request:

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.MyKit",
    "recipe": "collection.omni",
    "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
  }'
```

Only this endpoint checks a downloaded model's registry for updates, unless `do_not_upgrade` is `true`; loading a model on first use never does.

A `400` answers an invalid definition, an unknown model (with `code: "unknown_model"`), or offline mode (with `code: "lemond_offline"`), and nothing is registered.

### Import an Exported Model File

Files written by `lemonade export` (and the desktop app's Export button) are import-ready
`/v1/pull` request bodies — POST the file contents verbatim to register and install the model.
This works for regular models and collections alike; exported collection files additionally
carry `components` plus a `models` array embedding each component's definition (see the
`models` parameter above). For the file format and the export/import/Hugging Face workflows,
see [Share a collection](../guide/configuration/custom-models.md#share-a-collection-between-machines).

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF", "stream": true, "subscribe": false}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF", "stream": true, "subscribe": false}'
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF", "stream": true}'
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF", "stream": true}'
    ```

### Response format

=== "Non-streaming responses"

    `200`

    ```json
    {
      "status": "downloading",
      "model_name": "Qwen3-0.6B-GGUF",
      "id": "model:Qwen3-0.6B-GGUF",
      "type": "model",
      "running": true,
      "file": "",
      "file_index": 0,
      "total_files": 0,
      "bytes_downloaded": 0,
      "bytes_total": 0,
      "percent": 0,
      "total_download_size": 0,
      "bytes_previously_downloaded": 0,
      "completed_files_bytes": 0,
      "cumulative_bytes_downloaded": 0,
      "overall_bytes_downloaded": 0,
      "complete": false
    }
    ```

    **Field Descriptions:**

    - `message` - Only for local_import.
    - `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
    - `type` - Download type. Currently `model` or `backend` for server-owned jobs.
    - `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
    - `file`, `file_index`, `total_files` - Current file progress within the download.
    - `bytes_downloaded`, `bytes_total`, `percent` - Current-file byte progress as reported by the downloader.
    - `total_download_size` - Total expected bytes across all files when known.
    - `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
    - `completed_files_bytes` - Bytes from files completed before the current file.
    - `cumulative_bytes_downloaded`, `overall_bytes_downloaded` - Total bytes downloaded across the whole job. `overall_bytes_downloaded` is kept as a compatibility alias.
    - `complete` - `true` when the download completed successfully.
    - `error` - Error message, present only for failed jobs.
    - `code` - unknown_model when the model is not in the registry.

=== "Streaming responses"

    `200`

    ```text
    event: progress
    data: {"file": "Qwen3-0.6B-Q4_0.gguf", "file_index": 1, "total_files": 2, "bytes_downloaded": 0, "bytes_total": 382156480, "total_download_size": 382157232, "bytes_previously_downloaded": 0, "percent": 0}
    event: progress
    data: {"file": "Qwen3-0.6B-Q4_0.gguf", "file_index": 1, "total_files": 2, "bytes_downloaded": 382156480, "bytes_total": 382156480, "total_download_size": 382157232, "bytes_previously_downloaded": 382156480, "percent": 100}
    event: progress
    data: {"file": "config.json", "file_index": 2, "total_files": 2, "bytes_downloaded": 0, "bytes_total": 752, "total_download_size": 382157232, "bytes_previously_downloaded": 0, "percent": 0}
    event: progress
    data: {"file": "config.json", "file_index": 2, "total_files": 2, "bytes_downloaded": 752, "bytes_total": 752, "total_download_size": 382157232, "bytes_previously_downloaded": 752, "percent": 100}
    event: complete
    data: {"file": "", "file_index": 2, "total_files": 2, "bytes_downloaded": 0, "bytes_total": 0, "total_download_size": 0, "bytes_previously_downloaded": 0, "percent": 100}
    ```

    **Field Descriptions:**

    - `status` - Only on a complete event for an operation that had nothing to download.
    - `error` - Only on an error event.
    - `code` - unknown_model when the model is not in the registry.
<!-- END GENERATED: lemonade.pull -->

### Streaming Response (stream=true)

When `stream=true`, the endpoint returns Server-Sent Events with real-time download progress:

```
event: progress
data: {"file":"model.gguf","file_index":1,"total_files":2,"bytes_downloaded":1073741824,"bytes_total":2684354560,"percent":40}

event: progress
data: {"file":"config.json","file_index":2,"total_files":2,"bytes_downloaded":1024,"bytes_total":1024,"percent":100}

event: complete
data: {"file_index":2,"total_files":2,"percent":100}
```

**Event Types:**

| Event | Description |
|-------|-------------|
| `progress` | Sent during download with current file and byte progress |
| `complete` | Sent when all files are downloaded successfully |
| `error` | Sent if download fails, with `error` field containing the message |

### Server-owned download mode (`stream=true`, `subscribe=false`)

By default, `stream=true` keeps the `/v1/pull` HTTP response subscribed to Server-Sent Events until the download finishes. Clients that need download state to survive a renderer reload, tab close, or reconnect can also send `subscribe=false`.

When `stream=true` and `subscribe=false`, `/v1/pull` starts a server-owned model download job and returns a JSON snapshot immediately. The job continues on the server. Clients can poll [`GET /v1/downloads`](#get-v1downloads) to restore progress and can use [`POST /v1/downloads/control`](#post-v1downloadscontrol) to pause, cancel, or remove the job.

Example request:

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "stream": true,
    "subscribe": false
  }'
```

Example response:

```json
{
  "id": "model:Qwen3-0.6B-GGUF",
  "type": "model",
  "model_name": "Qwen3-0.6B-GGUF",
  "status": "downloading",
  "running": true,
  "file": "",
  "file_index": 0,
  "total_files": 0,
  "bytes_downloaded": 0,
  "bytes_total": 0,
  "total_download_size": 0,
  "bytes_previously_downloaded": 0,
  "completed_files_bytes": 0,
  "cumulative_bytes_downloaded": 0,
  "overall_bytes_downloaded": 0,
  "percent": 0,
  "complete": false
}
```

<!-- BEGIN GENERATED: lemonade.pull_variants -->
## `GET /v1/pull/variants`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Inspect a Hugging Face GGUF repository and enumerate the variants (quantizations and sharded folder groups) available for installation. Used by the `lemonade pull <owner/repo>` CLI flow and by the desktop app's model search to auto-populate the install form. The endpoint reads only public Hugging Face metadata; if the `HF_TOKEN` environment variable is set on the server, it is forwarded as a bearer token to access gated repositories.

Also served at `/api/v0/pull/variants`, `/api/v1/pull/variants` and `/v0/pull/variants`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `checkpoint` (query) | Yes | Hugging Face repo id, e.g. `unsloth/Qwen3-8B-GGUF`. Passed as a query string. A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-8B-GGUF"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-8B-GGUF"
    ```

### Response format

`200`

```json
{
  "checkpoint": "unsloth/Qwen3-8B-GGUF",
  "source": "huggingface",
  "recipe": "llamacpp",
  "repo_kind": "gguf",
  "suggested_name": "Qwen3-8B-GGUF",
  "suggested_labels": ["chat"],
  "mmproj_files": [],
  "draft_files": [],
  "variants": [
    {
      "name": "Q4_K_M",
      "primary_file": "Qwen3-8B-Q4_K_M.gguf",
      "files": ["Qwen3-8B-Q4_K_M.gguf"],
      "sharded": false,
      "size_bytes": 5027784512
    },
    {
      "name": "UD-Q4_K_XL",
      "primary_file": "Qwen3-8B-UD-Q4_K_XL.gguf",
      "files": ["Qwen3-8B-UD-Q4_K_XL.gguf"],
      "sharded": false,
      "size_bytes": 5135722304
    },
    {
      "name": "Q8_0",
      "primary_file": "Qwen3-8B-Q8_0.gguf",
      "files": ["Qwen3-8B-Q8_0.gguf"],
      "sharded": false,
      "size_bytes": 8709519168
    },
    {
      "name": "BF16",
      "primary_file": "Qwen3-8B-BF16.gguf",
      "files": ["Qwen3-8B-BF16.gguf"],
      "sharded": false,
      "size_bytes": 16388044384
    },
    {
      "name": "IQ4_NL",
      "primary_file": "Qwen3-8B-IQ4_NL.gguf",
      "files": ["Qwen3-8B-IQ4_NL.gguf"],
      "sharded": false,
      "size_bytes": 4793624384
    },
    {
      "name": "IQ4_XS",
      "primary_file": "Qwen3-8B-IQ4_XS.gguf",
      "files": ["Qwen3-8B-IQ4_XS.gguf"],
      "sharded": false,
      "size_bytes": 4581287744
    },
    {
      "name": "Q2_K",
      "primary_file": "Qwen3-8B-Q2_K.gguf",
      "files": ["Qwen3-8B-Q2_K.gguf"],
      "sharded": false,
      "size_bytes": 3281733440
    },
    {
      "name": "Q2_K_L",
      "primary_file": "Qwen3-8B-Q2_K_L.gguf",
      "files": ["Qwen3-8B-Q2_K_L.gguf"],
      "sharded": false,
      "size_bytes": 3427592000
    },
    {
      "name": "Q3_K_M",
      "primary_file": "Qwen3-8B-Q3_K_M.gguf",
      "files": ["Qwen3-8B-Q3_K_M.gguf"],
      "sharded": false,
      "size_bytes": 4124161856
    },
    {
      "name": "Q3_K_S",
      "primary_file": "Qwen3-8B-Q3_K_S.gguf",
      "files": ["Qwen3-8B-Q3_K_S.gguf"],
      "sharded": false,
      "size_bytes": 3769612096
    },
    {
      "name": "Q4_1",
      "primary_file": "Qwen3-8B-Q4_1.gguf",
      "files": ["Qwen3-8B-Q4_1.gguf"],
      "sharded": false,
      "size_bytes": 5247756096
    },
    {
      "name": "Q4_K_S",
      "primary_file": "Qwen3-8B-Q4_K_S.gguf",
      "files": ["Qwen3-8B-Q4_K_S.gguf"],
      "sharded": false,
      "size_bytes": 4802012992
    },
    {
      "name": "Q5_K_M",
      "primary_file": "Qwen3-8B-Q5_K_M.gguf",
      "files": ["Qwen3-8B-Q5_K_M.gguf"],
      "sharded": false,
      "size_bytes": 5851113280
    },
    {
      "name": "Q5_K_S",
      "primary_file": "Qwen3-8B-Q5_K_S.gguf",
      "files": ["Qwen3-8B-Q5_K_S.gguf"],
      "sharded": false,
      "size_bytes": 5720762176
    },
    {
      "name": "Q6_K",
      "primary_file": "Qwen3-8B-Q6_K.gguf",
      "files": ["Qwen3-8B-Q6_K.gguf"],
      "sharded": false,
      "size_bytes": 6725900096
    },
    {
      "name": "UD-IQ1_M",
      "primary_file": "Qwen3-8B-UD-IQ1_M.gguf",
      "files": ["Qwen3-8B-UD-IQ1_M.gguf"],
      "sharded": false,
      "size_bytes": 2396489536
    },
    {
      "name": "UD-IQ1_S",
      "primary_file": "Qwen3-8B-UD-IQ1_S.gguf",
      "files": ["Qwen3-8B-UD-IQ1_S.gguf"],
      "sharded": false,
      "size_bytes": 2275379008
    },
    {
      "name": "UD-IQ2_M",
      "primary_file": "Qwen3-8B-UD-IQ2_M.gguf",
      "files": ["Qwen3-8B-UD-IQ2_M.gguf"],
      "sharded": false,
      "size_bytes": 3110897472
    },
    {
      "name": "UD-IQ2_XXS",
      "primary_file": "Qwen3-8B-UD-IQ2_XXS.gguf",
      "files": ["Qwen3-8B-UD-IQ2_XXS.gguf"],
      "sharded": false,
      "size_bytes": 2605221696
    },
    {
      "name": "UD-IQ3_XXS",
      "primary_file": "Qwen3-8B-UD-IQ3_XXS.gguf",
      "files": ["Qwen3-8B-UD-IQ3_XXS.gguf"],
      "sharded": false,
      "size_bytes": 3410265920
    },
    {
      "name": "UD-Q2_K_XL",
      "primary_file": "Qwen3-8B-UD-Q2_K_XL.gguf",
      "files": ["Qwen3-8B-UD-Q2_K_XL.gguf"],
      "sharded": false,
      "size_bytes": 3501975360
    },
    {
      "name": "UD-Q3_K_XL",
      "primary_file": "Qwen3-8B-UD-Q3_K_XL.gguf",
      "files": ["Qwen3-8B-UD-Q3_K_XL.gguf"],
      "sharded": false,
      "size_bytes": 4307052352
    },
    {
      "name": "UD-Q5_K_XL",
      "primary_file": "Qwen3-8B-UD-Q5_K_XL.gguf",
      "files": ["Qwen3-8B-UD-Q5_K_XL.gguf"],
      "sharded": false,
      "size_bytes": 5878081344
    },
    {
      "name": "UD-Q6_K_XL",
      "primary_file": "Qwen3-8B-UD-Q6_K_XL.gguf",
      "files": ["Qwen3-8B-UD-Q6_K_XL.gguf"],
      "sharded": false,
      "size_bytes": 7490549568
    },
    {
      "name": "UD-Q8_K_XL",
      "primary_file": "Qwen3-8B-UD-Q8_K_XL.gguf",
      "files": ["Qwen3-8B-UD-Q8_K_XL.gguf"],
      "sharded": false,
      "size_bytes": 10824038208
    }
  ]
}
```

**Field Descriptions:**

- `checkpoint` - Echoed input (normalized to `owner/repo` when given as a URL).
- `source` - Registry the variants came from.
- `recipe` - Suggested recipe (`llamacpp` for a GGUF repo, `ryzenai-llm` for an ONNX RyzenAI repo, `collection.omni` for an exported collection; future expansion may return other values).
- `suggested_name` - Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name.
- `suggested_labels` - Inferred labels — `vision` if any `mmproj-*.gguf` files exist, `mtp` or `dflash` if a variant ships a draft model, plus `embeddings`/`reranking` if those substrings appear in the repo id, and the recipe's deployment label (e.g. `chat`) when no other label names one.
- `mmproj_files` - Bare filenames of `mmproj-*.gguf` files in the repo; the first one should be passed as `mmproj` to `/v1/pull` for vision models.
- `draft_files` - Bare names of draft-model GGUF files.
- `variants` - Quantizations for the repo. Each entry has `name` (e.g. `Q4_K_M`, `UD-Q4_K_XL`), `primary_file`, `files`, `sharded`, and `size_bytes` (from the HF `?blobs=true` listing). Ranked by frequency of use in `server_models.json` (`Q4_K_M`, `UD-Q4_K_XL`, `Q8_0`, `Q4_0` first, everything else sorted lexicographically). The CLI `lemonade pull` menu also accepts a free-text variant name.
  - `draft_file` - The draft model paired with this variant, when the repository ships one.
- `size` - collection only: the collection's total size, from its manifest.
- `component_count` - collection only: how many models the collection bundles.
<!-- END GENERATED: lemonade.pull_variants -->

### Error responses

| Status | Cause |
|--------|-------|
| 400 | `checkpoint` query parameter missing or malformed (must contain `/`). |
| 404 | Hugging Face returned 404 for the checkpoint. |
| 500 | Other transport or parsing failures; the response body contains an `error` message. |

A `400` also answers a `source` that disagrees with the checkpoint URL's registry, or offline mode (`code: lemond_offline`).

<!-- BEGIN GENERATED: lemonade.registry_search -->
## `GET /v1/registry/search`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Search a remote model registry (Hugging Face or ModelScope) for repositories matching a text query. This endpoint returns **candidate repositories** based on registry metadata; it does not verify that a repository contains servable files. The desktop app's Model Manager follows up with [`/v1/pull/variants`](#get-v1pullvariants) on each candidate and only offers a download for repositories whose file listing passes that validation.

Requires network access: returns 400 with code `lemond_offline` when the server is in offline mode.

Also served at `/api/v0/registry/search`, `/api/v1/registry/search` and `/v0/registry/search`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` (query) | Yes | Search text, minimum 3 characters after trimming. `q` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry to search: `huggingface` (default) or `modelscope`. Aliases `hf` and `ms` are accepted; the canonical name is echoed in the response. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `limit` (query) | No | Maximum number of results, an integer from 1 to 50. Default 12. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` (query) | No | The only accepted value is `gguf`. Biases search and ranking toward GGUF repositories and echoes `"format": "gguf"` in the response. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/registry/search?source=modelscope&query=qwen&format=gguf"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/registry/search?source=modelscope&query=qwen&format=gguf"
    ```

### Response format

`200`

```json
{
  "source": "modelscope",
  "query": "qwen",
  "format": "gguf",
  "total": 3277,
  "results": [
    {
      "repository_id": "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF",
      "display_name": "Qwen3.8-Flash-Next-GSQ-RCO-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:apache-2.0",
        "library:gguf",
        "library:pytorch",
        "task:image-text-to-text",
        "custom_tag:gguf",
        "custom_tag:gsq",
        "custom_tag:rco",
        "custom_tag:quantization",
        "custom_tag:mixed-precision",
        "custom_tag:ist-daslab",
        "custom_tag:moe",
        "custom_tag:multimodal",
        "custom_tag:vision"
      ],
      "task": "image-text-to-text",
      "downloads": 5432220,
      "likes": 30,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen1.5-7B-Chat",
      "display_name": "千问1.5-7B-Chat",
      "source": "modelscope",
      "repository_type": "model",
      "description": "Qwen1.5版本本次开源了包括0.5B、1.8B、4B、7B、14B和72B在内的六种大小的基础和聊天模型，同时，也开源了量化模型。不仅提供了Int4和Int8的GPTQ模型，还有AWQ模型，以及GGUF量化模型。为了提升开发者体验，Qwen1.5的代码合并到Hugging Face Transformers中，开发者现在可以直接使用transformers>=4.37.0而无需trust_remote_code。此外，Qwen1.5支持了例如vLLM、SGLang、AutoGPTQ等框架对Qwen1.5的支持。Qwen1.5显著提升了聊天模型与人类偏好的一致性，并且改善了它们的多语言能力。所有模型提供了统一的上下文长度支持，支持32K上下文, 基础语言模型的质量也有所改进。",
      "tags": [
        "license:other",
        "model_type:qwen2",
        "library:transformer",
        "library:safetensors",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:chat",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 1409896,
      "likes": 168,
      "has_gguf": true
    },
    {
      "repository_id": "unsloth/Qwen3-30B-A3B-GGUF",
      "display_name": "Qwen3-30B-A3B-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:apache-2.0",
        "model_type:qwen3_moe",
        "library:transformer",
        "library:gguf",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:qwen3",
        "custom_tag:qwen",
        "custom_tag:unsloth",
        "custom_tag:transformers",
        "custom_tag:gguf",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 1120829,
      "likes": 16,
      "has_gguf": true
    },
    {
      "repository_id": "unsloth/Qwen3.6-35B-A3B-GGUF",
      "display_name": "Qwen3.6-35B-A3B-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:apache-2.0",
        "library:gguf",
        "library:pytorch",
        "task:image-text-to-text",
        "custom_tag:unsloth",
        "custom_tag:qwen",
        "custom_tag:qwen3_5_moe",
        "custom_tag:gguf"
      ],
      "task": "image-text-to-text",
      "downloads": 435575,
      "likes": 133,
      "has_gguf": true
    },
    {
      "repository_id": "unsloth/Qwen3.8-Flash-Next-GGUF",
      "display_name": "Qwen3.8-Flash-Next-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:other",
        "library:gguf",
        "library:pytorch",
        "task:image-text-to-text",
        "custom_tag:unsloth",
        "custom_tag:gguf"
      ],
      "task": "image-text-to-text",
      "downloads": 422506,
      "likes": 57,
      "has_gguf": true
    },
    {
      "repository_id": "unsloth/Qwen3.8-27B-GGUF",
      "display_name": "Qwen3.8-27B-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:apache-2.0",
        "model_type:qwen3_5",
        "library:gguf",
        "library:pytorch",
        "custom_tag:unsloth",
        "custom_tag:gguf"
      ],
      "task": "",
      "downloads": 406958,
      "likes": 302,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen1.5-0.5B-Chat",
      "display_name": "千问1.5-0.5B-Chat",
      "source": "modelscope",
      "repository_type": "model",
      "description": "Qwen1.5版本本次开源了包括0.5B、1.8B、4B、7B、14B和72B在内的六种大小的基础和聊天模型，同时，也开源了量化模型。不仅提供了Int4和Int8的GPTQ模型，还有AWQ模型，以及GGUF量化模型。为了提升开发者体验，Qwen1.5的代码合并到Hugging Face Transformers中，开发者现在可以直接使用transformers>=4.37.0而无需trust_remote_code。此外，Qwen1.5支持了例如vLLM、SGLang、AutoGPTQ等框架对Qwen1.5的支持。Qwen1.5显著提升了聊天模型与人类偏好的一致性，并且改善了它们的多语言能力。所有模型提供了统一的上下文长度支持，支持32K上下文, 基础语言模型的质量也有所改进。",
      "tags": [
        "license:other",
        "model_type:qwen2",
        "library:transformer",
        "library:safetensors",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:chat",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 371658,
      "likes": 71,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen3-8B-GGUF",
      "display_name": "千问3-8B-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:apache-2.0",
        "library:gguf",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:gguf",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 325784,
      "likes": 60,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen2.5-3B-Instruct-GGUF",
      "display_name": "千问2.5-3B-Instruct-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:other",
        "library:gguf",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:chat",
        "custom_tag:gguf",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 323057,
      "likes": 46,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen1.5-0.5B-Chat-GPTQ-Int8",
      "display_name": "千问1.5-0.5B-Chat-GPTQ-Int8",
      "source": "modelscope",
      "repository_type": "model",
      "description": "Qwen1.5版本本次开源了包括0.5B、1.8B、4B、7B、14B和72B在内的六种大小的基础和聊天模型，同时，也开源了量化模型。不仅提供了Int4和Int8的GPTQ模型，还有AWQ模型，以及GGUF量化模型。为了提升开发者体验，Qwen1.5的代码合并到Hugging Face Transformers中，开发者现在可以直接使用transformers>=4.37.0而无需trust_remote_code。此外，Qwen1.5支持了例如vLLM、SGLang、AutoGPTQ等框架对Qwen1.5的支持。Qwen1.5显著提升了聊天模型与人类偏好的一致性，并且改善了它们的多语言能力。所有模型提供了统一的上下文长度支持，支持32K上下文, 基础语言模型的质量也有所改进。",
      "tags": [
        "license:other",
        "model_type:qwen2",
        "library:transformer",
        "library:safetensors",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:chat",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 320739,
      "likes": 6,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen2.5-7B-Instruct-GGUF",
      "display_name": "千问2.5-7B-Instruct-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "",
      "tags": [
        "license:apache-2.0",
        "library:gguf",
        "task:text-generation",
        "custom_tag:chat",
        "custom_tag:gguf",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 256996,
      "likes": 94,
      "has_gguf": true
    },
    {
      "repository_id": "Qwen/Qwen1.5-72B-Chat-GPTQ-Int4",
      "display_name": "千问1.5-72B-Chat-GPTQ-Int4",
      "source": "modelscope",
      "repository_type": "model",
      "description": "Qwen1.5版本本次开源了包括0.5B、1.8B、4B、7B、14B和72B在内的六种大小的基础和聊天模型，同时，也开源了量化模型。不仅提供了Int4和Int8的GPTQ模型，还有AWQ模型，以及GGUF量化模型。为了提升开发者体验，Qwen1.5的代码合并到Hugging Face Transformers中，开发者现在可以直接使用transformers>=4.37.0而无需trust_remote_code。此外，Qwen1.5支持了例如vLLM、SGLang、AutoGPTQ等框架对Qwen1.5的支持。Qwen1.5显著提升了聊天模型与人类偏好的一致性，并且改善了它们的多语言能力。所有模型提供了统一的上下文长度支持，支持32K上下文, 基础语言模型的质量也有所改进。",
      "tags": [
        "license:other",
        "model_type:qwen2",
        "library:transformer",
        "library:safetensors",
        "library:pytorch",
        "task:text-generation",
        "custom_tag:chat",
        "deploy:swingdeploy"
      ],
      "task": "text-generation",
      "downloads": 201742,
      "likes": 26,
      "has_gguf": true
    }
  ]
}
```

**Field Descriptions:**

- `source`, `query` - Echoed input (`source` canonicalized to `huggingface` or `modelscope`).
- `format` - Present only when `format=gguf` was requested.
- `total` - Total match count reported by the upstream registry; may exceed the number of returned results.
- `results` - Up to `limit` repositories, each with `repository_id`, `display_name`, `source`, `repository_type`, `description`, `tags`, `task`, `downloads`, `likes`, and `has_gguf`. `has_gguf` is a hint derived from registry metadata, not proof of a servable model — [`/v1/pull/variants`](#get-v1pullvariants) performs the authoritative file-level validation.
<!-- END GENERATED: lemonade.registry_search -->

### Error responses

| Status | Cause |
|--------|-------|
| 400 | `query` shorter than 3 characters, invalid `source`, `limit`, or `format`, or the server is in offline mode (`code: lemond_offline`). |
| 429 | The upstream registry rate-limited the request. |
| 502 | Other upstream transport or parsing failures; the body includes the upstream status code when available. |

<!-- BEGIN GENERATED: lemonade.delete -->
## `POST /v1/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Delete a model by removing it from local storage. If the model is currently loaded, it will be unloaded first.

> Note: deleting a collection (`recipe: "collection.omni"`) removes only the collection entry from `user_models.json`; its components stay on disk. Delete the components individually if you want to free their disk space.

Also served at `/api/v0/delete`, `/api/v1/delete` and `/v0/delete`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to delete. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A file still held open, for example by a download that was just cancelled, is retried up to 3 times, 5 seconds apart. An unknown model answers `422`; other failures answer `500`.

### Example request

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

### Response format

`200`

```json
{"status": "success", "message": "Deleted model: user.Phi-4-Mini-GGUF"}
```
<!-- END GENERATED: lemonade.delete -->

In case of an error, the `error` field will contain the error message.

<!-- BEGIN GENERATED: lemonade.load -->
## `POST /v1/load`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly load a registered model into memory. This is useful to ensure that the model is loaded before you make a request. Installs the model if necessary.

> Note: loading a collection (`recipe: "collection.omni"`) loads each of its components in turn. Per-model options like `ctx_size` or `llamacpp_backend` are not forwarded to components — set them on each component's own `recipe_options.json` entry instead.

Recipe option fields on `/v1/load` have three-state semantics. Omitting a field keeps using its saved per-model value. Passing explicit `null` ignores only that saved key for this load and falls through to the lower default layers without changing `recipe_options.json`. Passing a concrete value overrides the saved value. For `*_args`, a concrete value replaces the model/architecture args scope for that load; backend/machine args remain only when `merge_args` is true. `ctx_size: -1` is a concrete value meaning automatic context sizing, not a tombstone. With `save_options: true`, concrete values are persisted as usual while a `null` tombstone preserves the existing saved value for that key.

Also served at `/api/v0/load`, `/api/v1/load` and `/v0/load`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to load. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `pinned` | No | Boolean. If true, pins the loaded model to prevent LRU eviction. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `save_options` | No | Boolean. If true, saves recipe options to `recipe_options.json`. Any previously stored value for `model_name` is replaced. To save options without loading, or to change one option without resending the rest, use [`POST /v1/models/{id}/options`](openai.md#post-v1modelsidoptions) instead. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | llamacpp, flm, ryzenai-llm: Context size for the model. Overrides the default value. Pass `-1` to size it automatically instead of using a saved value; omit it to use the saved value. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_backend` | No | llamacpp: LlamaCpp backend to use (`vulkan`, `rocm`, `metal` or `cpu`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_args` | No | llamacpp: Custom arguments to pass to llama-server. The following are NOT allowed: `-m`, `--port`, `--ctx-size`, `-ngl`, `--jinja`, `--mmproj`, `--embeddings`, `--reranking`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_backend` | No | whispercpp: WhisperCpp backend: `npu` or `cpu` on Windows; `cpu` or `vulkan` on Linux. Default is `npu` if supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_args` | No | whispercpp: Custom arguments to pass to whisper-server. The following are NOT allowed: `-m`, `--model`, `--port`. Example: `--convert`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | sd-cpp: Number of inference steps for image generation. Default: 20. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | sd-cpp: Classifier-free guidance scale for image generation. Default: 7.0. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `width` | No | sd-cpp: Image width in pixels. Default: 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | sd-cpp: Image height in pixels. Default: 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `merge_args` | No | Boolean. If true (default), backend/machine `*_args` are inherited; concrete request `*_args` replace model/architecture args while keeping backend args. If false, no inherited custom args or overridable runtime defaults are applied. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

**Setting Priority:**

When loading a model, settings are applied in this priority order:
1. Values explicitly passed in the `load` request (highest priority)
2. Per-model values configurable in `recipe_options.json` (see below for details)
3. Values from environment variables or server startup arguments (see [Server Configuration](../guide/configuration/README.md))
4. Default hardcoded values in `lemond` (lowest priority)

Loading a model that is already loaded with the same options does nothing; different options reload it.

A `collection.router` model loads nothing until a request routes to a candidate.

Every option the model's recipe accepts can be passed; the [Backend Reference](../dev/backends-reference.md#recipe-options) lists them per recipe. Load failures answer with the same error object as inference requests: `404` for an unknown or unsupported model, `409` when pinned models fill every slot, and `500` when the backend fails to start.

### Per-model options

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

### Example request

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

### Response format

`200`

```json
{
  "status": "success",
  "model_name": "Qwen3-0.6B-GGUF",
  "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
  "recipe": "llamacpp"
}
```
<!-- END GENERATED: lemonade.load -->

In case of an error, the `error` field will contain the error message.

### Example requests

Basic load:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF"
  }'
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

Explicitly unload a model from memory. This is useful to free up memory while still leaving the server process running (which takes minimal resources but a few seconds to start).

Also served at `/api/v0/unload`, `/api/v1/unload` and `/v0/unload`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | No | Name of the specific model to unload. If not provided, all loaded models will be unloaded. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The body is optional: an empty or unparsable body unloads every model. A model that is not loaded answers `404`.

### Example request

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

### Response format

`200`

```json
{"status": "success", "message": "Model unloaded successfully", "model_name": "Qwen3-0.6B-GGUF"}
```

**Field Descriptions:**

- `model_name` - Only when one model was unloaded.
<!-- END GENERATED: lemonade.unload -->

Error response (model not loaded):

```json
{
  "error": "Model not loaded: Qwen3-0.6B-GGUF"
}
```

In case of an error, the `error` field will contain the error message.

### Example requests

Unload all models:

```bash
curl -X POST http://localhost:13305/v1/unload
```

<!-- BEGIN GENERATED: lemonade.downloads -->
## `GET /v1/downloads`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

List server-owned model download jobs that were started with `POST /v1/pull` or `POST /v1/install` using `stream=true` and `subscribe=false`.

This endpoint is intended for clients that need to restore download-manager state after a reload or reconnect. Active, paused, cancelled, and errored jobs remain visible until the client removes them. Completed jobs remain visible briefly so clients can observe completion and refresh model state.

Also served at `/api/v0/downloads`, `/api/v1/downloads` and `/v0/downloads`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

Completed jobs stay listed for 30 seconds.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/downloads"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/downloads
    ```

### Response format

`200`

```json
[
  {
    "id": "model:Qwen3-0.6B-GGUF",
    "type": "model",
    "model_name": "Qwen3-0.6B-GGUF",
    "status": "downloading",
    "running": true,
    "file": "",
    "file_index": 0,
    "total_files": 0,
    "bytes_downloaded": 0,
    "bytes_total": 0,
    "percent": 0,
    "total_download_size": 0,
    "bytes_previously_downloaded": 0,
    "completed_files_bytes": 0,
    "cumulative_bytes_downloaded": 0,
    "overall_bytes_downloaded": 0,
    "complete": false
  }
]
```

**Field Descriptions:**

- `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
- `type` - Download type. Currently `model` or `backend` for server-owned jobs.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `status` - Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file`, `file_index`, `total_files` - Current file progress within the download.
- `bytes_downloaded`, `bytes_total`, `percent` - Current-file byte progress as reported by the downloader.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded`, `overall_bytes_downloaded` - Total bytes downloaded across the whole job. `overall_bytes_downloaded` is kept as a compatibility alias.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - unknown_model when the model is not in the registry.
<!-- END GENERATED: lemonade.downloads -->

<!-- BEGIN GENERATED: lemonade.downloads_control -->
## `POST /v1/downloads/control`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Control a server-owned model download job.

Also served at `/api/v0/downloads/control`, `/api/v1/downloads/control` and `/v0/downloads/control`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` | Yes | Download id returned by `POST /v1/pull` or `GET /v1/downloads`, for example `model:Qwen3-0.6B-GGUF`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `action` | Yes | One of `pause`, `cancel`, or `remove`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Actions

| Action | Description |
|--------|-------------|
| `pause` | Requests the worker to stop and keeps the job visible as `paused`. The worker may briefly report `running=true` while it unwinds. |
| `cancel` | Requests the worker to stop and marks the job as `cancelled`. Clients should wait for `running=false` before deleting partial files. |
| `remove` | Removes a stopped job from the server registry. If the worker is still running, the server keeps the job visible and treats the request as a cancel request until the worker stops. |

A `pause` or `cancel` for a job that already finished leaves it as it is.

A `400` answers a missing `id` or `action`, invalid JSON, or an unknown action. A `pause` or `cancel` for a job that is not listed answers `404`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/downloads/control" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"id": "model:Qwen3-0.6B-GGUF", "action": "pause"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/downloads/control \
      -H "Content-Type: application/json" \
      -d '{"id": "model:Qwen3-0.6B-GGUF", "action": "pause"}'
    ```

### Response format

`200`

```json
{
  "status": "paused",
  "id": "model:Qwen3-0.6B-GGUF",
  "type": "model",
  "model_name": "Qwen3-0.6B-GGUF",
  "running": true,
  "file": "",
  "file_index": 0,
  "total_files": 0,
  "bytes_downloaded": 0,
  "bytes_total": 0,
  "percent": 0,
  "total_download_size": 0,
  "bytes_previously_downloaded": 0,
  "completed_files_bytes": 0,
  "cumulative_bytes_downloaded": 0,
  "overall_bytes_downloaded": 0,
  "complete": false
}
```

**Field Descriptions:**

- `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
- `type` - Download type. Currently `model` or `backend` for server-owned jobs.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file`, `file_index`, `total_files` - Current file progress within the download.
- `bytes_downloaded`, `bytes_total`, `percent` - Current-file byte progress as reported by the downloader.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded`, `overall_bytes_downloaded` - Total bytes downloaded across the whole job. `overall_bytes_downloaded` is kept as a compatibility alias.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - unknown_model when the model is not in the registry.
<!-- END GENERATED: lemonade.downloads_control -->

For `pause` and `cancel`, the endpoint returns the latest job snapshot:

```json
{
  "id": "model:Qwen3-0.6B-GGUF",
  "type": "model",
  "model_name": "Qwen3-0.6B-GGUF",
  "status": "paused",
  "running": false,
  "file": "model.gguf",
  "file_index": 1,
  "total_files": 2,
  "bytes_downloaded": 1073741824,
  "bytes_total": 2684354560,
  "percent": 40,
  "complete": false
}
```

For `remove`, the endpoint returns:

```json
{"status":"ok"}
```

If the job is already missing and `action` is `remove`, the endpoint returns:

```json
{"status":"ok","missing":true}
```

<!-- BEGIN GENERATED: lemonade.classify -->
## `POST /v1/classify`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Run an encoder text-classifier (PII, prompt-safety, domain, etc.) on an input string and return per-label scores in `[0, 1]`. The target model must use the `onnxruntime` recipe. Both sequence-classification (one label set) and token-classification (aggregated span labels) models are supported.

**Supported architectures:** single-sequence encoder families — BERT, DistilBERT, RoBERTa, XLM-RoBERTa, DeBERTa (v1/v2), ELECTRA, ALBERT, CamemBERT. A stock `optimum-cli export onnx` directory of one of these works as-is.

A servable model directory is `model.onnx` + `tokenizer.json` + `config.json`. The `config.json` is **always required**: it declares the architecture, which is checked against the list above so an unsupported family (e.g. XLNet, which uses different segment/special-token conventions) is **rejected at load time** rather than served with wrong scores. The output contract (labels, normalization, token budget) is read from that same config; an optional `manifest.json` overrides it but does not replace the config. Without a manifest, inference assumes **single-label softmax**; a multi-label (sigmoid) model must declare `problem_type: multi_label_classification` in its config or ship a `manifest.json`.

This endpoint provides the classification capability that the router's `classifier` condition type consumes.

Also served at `/api/v0/classify`, `/api/v1/classify` and `/v0/classify`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | No | Classifier model id (a model with the `onnxruntime` recipe). Optional when exactly one classification model is already loaded; the loaded model is used and echoed in the response. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to classify. `text` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Return only the highest-scoring `k` labels. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `text` | No | Alias for `input`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

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

### Response format

`200`

```json
{
  "object": "classification",
  "model": "Phishing-Email-Detection-ONNX",
  "labels": {
    "LABEL_0": 5.558096745517105e-05,
    "LABEL_1": 0.9999443888664246,
    "LABEL_2": 7.880903929446959e-09,
    "LABEL_3": 2.331440818181818e-08
  }
}
```

**Field Descriptions:**

- `model` - The model that classified the input, including one chosen because it was the only one loaded.
- `labels` - Score in [0, 1] for each label.
<!-- END GENERATED: lemonade.classify -->

Label names come from the model's `id2label` — from `config.json`, or from `manifest.json` when one is present to override it; some upstream models only declare generic `LABEL_<n>` names — see the model card for their meaning.

Malformed requests (invalid JSON, missing `input`/`text`, non-string fields, non-positive `top_k`) return `400` with an `error` object before any model is loaded.

<!-- BEGIN GENERATED: lemonade.audio_generations -->
## `POST /v1/audio/generations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Audio Generation API. You provide a text prompt and receive a generated audio clip. The loaded model decides the kind of audio: music with ACE-Step models (e.g. `ACE-Step-Music`), sound effects with ThinkSound models (e.g. `ThinkSound-SFX`).

This endpoint is not part of the OpenAI API (OpenAI's audio endpoints cover speech and transcription only), so it is a Lemonade-specific extension.

> **Performance:** generation runs on the GPU (Vulkan, ROCm, or CUDA) and takes from seconds (short sound effects) to minutes (full-length music) depending on duration and hardware.

Also served at `/api/v0/audio/generations`, `/api/v1/audio/generations` and `/v0/audio/generations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The audio-generation model to use (e.g., `ThinkSound-SFX`, `ACE-Step-Music`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text description of the music or sound effect to generate. For music, this is the style description: genre, mood, tempo, instruments, and voice. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `lyrics` | No | Lyrics to sing (ACE-Step only). When present and not empty, the track is generated with vocals singing these lyrics. Omitting the field, an empty string, or the sentinel `[Instrumental]` (any case) produces an instrumental track. See [Lyrics](#lyrics) below for the expected format. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vocal_language` | No | BCP-47 language code of the lyrics, e.g. `en`, `fr`, `ja` (ACE-Step only). Default: `en`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `duration` | No | Length of the clip in seconds. Defaults to the backend's native default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Number of inference steps. Lower is faster, higher can improve quality. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg` | No | Classifier-free guidance strength (ThinkSound only). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only formats the backend natively produces are accepted (currently `wav`); other values are rejected with `400 Bad Request`. Default: `wav`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Lyrics

ACE-Step vocals are a two-stage pipeline inside the backend: a language model first turns the style description and lyrics into audio codes, then the diffusion synthesizer renders those codes into audio. The instrumental path skips the language-model stage entirely, which also means lyrics embedded in the `prompt` field are treated as style text — they are never sung. Vocal generations take noticeably longer than instrumental ones of the same duration because of the extra language-model pass.

Format the `lyrics` value the way the ACE-Step authors recommend:

- Mark each song section with a structure tag on its own line: `[verse]`, `[chorus]`, `[bridge]`, `[intro]`, `[outro]`.
- Write one sung phrase per line and separate sections with a blank line.
- Describe the voice ("gentle female vocals", "raspy male baritone") in `prompt`, not in the lyrics.
- Lyrics may be in any supported language; set `vocal_language` to match.

### Response

On success the raw audio bytes are returned with the matching content type (`audio/wav`). On failure the response is JSON with an `error` object: `400` for invalid requests, `404` for unknown models, `500` when the backend reports an error, and `502` when the backend produces no output.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/generations" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "ThinkSound-SFX",
          "prompt": "glass shattering on a stone floor",
          "duration": 5,
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
          "duration": 5,
          "seed": 42
        }' \
      --output output.wav
    ```

### Response format

`200`, `Content-Type: audio/wav`, 884,780 bytes
<!-- END GENERATED: lemonade.audio_generations -->

### Example request (music with vocals)

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

3D Generation API. You provide an input image and receive a textured 3D mesh as a glTF-binary (`.glb`) file. Serves TRELLIS models (e.g. `TRELLIS-3D`). The input image must be PNG, JPEG, BMP, or GIF.

This endpoint is not part of the OpenAI API, so it is a Lemonade-specific extension.

> **Performance:** 3D reconstruction runs on the GPU (Vulkan, ROCm, or CUDA) and takes on the order of minutes; higher cascade resolutions take longer.

Also served at `/api/v0/3d/generations`, `/api/v1/3d/generations` and `/v0/3d/generations`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The 3D-generation model to use (e.g., `TRELLIS-3D`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | Base64-encoded input image (optionally a `data:` URL). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `resolution` | No | Cascade resolution: `512`, `1024`, or `1536`. Default: `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `bg_removal` | No | Background removal mode: `threshold` or `birefnet`. Use `birefnet` for photos with real backgrounds. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `uv` | No | UV atlas method: `xatlas` (default) or `box`. `xatlas` runs a full UV unwrap giving every face unique atlas space — best quality, but chart computation is superlinear in face count. `box` is a faster 6-plane projection with occlusion-aware bucket assignment and depth-tested rasterization; small texture artifacts remain possible in concave regions. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only formats the backend natively produces are accepted (currently `glb`); other values are rejected with `400 Bad Request`. Default: `glb`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response

On success the raw mesh bytes are returned as `model/gltf-binary`. On failure the response is JSON with an `error` object: `400` for invalid requests, `404` for unknown models, `500` when the backend reports an error, and `502` when the backend produces no output.

### Example request

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

### Response format

`200`, `Content-Type: model/gltf-binary`, 4,534,160 bytes
<!-- END GENERATED: lemonade.3d_generations -->

<!-- BEGIN GENERATED: lemonade.install -->
## `POST /v1/install`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Install or update a backend for a specific recipe/backend pair, **or** register a cloud OpenAI-compatible provider. The request body is dispatched by the `backend` field: any value other than `"cloud"` is treated as a local backend install.

Also served at `/api/v0/install`, `/api/v1/install` and `/v0/install`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: Recipe name (for example, `llamacpp`, `flm`, `whispercpp`, `sd-cpp`, `ryzenai-llm`) | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Local backends: Backend name within the recipe (for example, `vulkan`, `rocm`, `cpu`, `default`). Cloud providers: Must be the literal string `"cloud"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Local backends: If `true`, returns Server-Sent Events with progress. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `force` | No | Local backends: If `true`, bypasses hardware filtering for `unsupported` backends and attempts installation anyway. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, required: Short identifier (e.g. `fireworks`). Used as the model-name prefix. Must match `[a-z0-9_-]+`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `base_url` | No | Cloud providers, required: OpenAI-compatible base URL ending in `/v1` (or equivalent). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | No | Cloud providers: Optional. If set, stored in process memory; honors env-wins precedence (see `/v1/cloud/auth`). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Cloud providers: Default `false`. Must be `true` to send an API key to an `http://` base URL. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_name` | No | Cloud providers: Header carrying the API key. Must be a valid HTTP header name. Default `"Authorization"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_prefix` | No | Cloud providers: Value prefix before the key. Default `"Bearer "`; pass `""` for gateways that expect the bare key. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `wire_format` | No | Cloud providers: `"openai"` (default) or `"anthropic"`. An `"anthropic"` provider is served from `POST /v1/messages` only; any other value returns 400. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Local backends with `stream: true`: `false` starts a server-owned download and answers with its snapshot at once. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A backend this system does not support is refused with `400` unless `force` is set, and one that needs manual setup answers with the setup guide's URL instead of installing.

`stream: true` sends progress as server-sent events; with `subscribe: false` as well, the server owns the download, which [`GET /v1/downloads`](#get-v1downloads) and [`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control.

A local install without both `recipe` and `backend`, or a backend this system does not support, is answered with `400` and an `error` string; an invalid cloud provider field is answered with `400` and an `error` object. Any other failure, including invalid JSON, is answered with `500` and an `error` string.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "backend": "cloud",
          "provider": "fireworks",
          "base_url": "https://api.fireworks.ai/inference/v1"
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/install \
      -H "Content-Type: application/json" \
      -d '{
          "backend": "cloud",
          "provider": "fireworks",
          "base_url": "https://api.fireworks.ai/inference/v1"
        }'
    ```

=== "PowerShell (streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"recipe": "llamacpp", "backend": "vulkan", "stream": true}'
    ```

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/v1/install \
      -H "Content-Type: application/json" \
      -d '{"recipe": "llamacpp", "backend": "vulkan", "stream": true}'
    ```

### Response format

=== "Non-streaming responses"

    `200`

    ```json
    {
      "status": "success",
      "backend": "cloud",
      "provider": "fireworks",
      "base_url": "https://api.fireworks.ai/inference/v1",
      "allow_insecure_http": false,
      "auth_header_name": "Authorization",
      "auth_header_prefix": "Bearer ",
      "wire_format": "openai",
      "models_discovered": 0,
      "auth_state": {"env_var_set": false, "runtime_key_set": false}
    }
    ```

    **Field Descriptions:**

    - `models_discovered` - Chat models discovered with the resolved API key.
    - `auth_state`
      - `env_var_set` - Whether LEMONADE_<PROVIDER>_API_KEY is set for lemond.
      - `runtime_key_set` - Whether a key is held in lemond's memory.
    - `warning` - The warnings joined into one string, for older clients.
    - `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
    - `type` - Download type. Currently `model` or `backend` for server-owned jobs.
    - `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
    - `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
    - `file`, `file_index`, `total_files` - Current file progress within the download.
    - `bytes_downloaded`, `bytes_total`, `percent` - Current-file byte progress as reported by the downloader.
    - `total_download_size` - Total expected bytes across all files when known.
    - `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
    - `completed_files_bytes` - Bytes from files completed before the current file.
    - `cumulative_bytes_downloaded`, `overall_bytes_downloaded` - Total bytes downloaded across the whole job. `overall_bytes_downloaded` is kept as a compatibility alias.
    - `complete` - `true` when the download completed successfully.
    - `error` - Error message, present only for failed jobs.
    - `code` - unknown_model when the model is not in the registry.

=== "Streaming responses"

    `200`

    ```text
    event: complete
    data: {"file": "llama-b11393-bin-ubuntu-vulkan-x64.tar.gz", "file_index": 1, "total_files": 1, "bytes_downloaded": 0, "bytes_total": 0, "total_download_size": 0, "bytes_previously_downloaded": 0, "percent": 100}
    ```

    **Field Descriptions:**

    - `status` - Only on a complete event for an operation that had nothing to download.
    - `error` - Only on an error event.
    - `code` - unknown_model when the model is not in the registry.
<!-- END GENERATED: lemonade.install -->

### Install a local backend

If the backend is already installed but outdated, this endpoint updates it to the configured version.

Example request:

```bash
curl -X POST http://localhost:13305/v1/install \
  -H "Content-Type: application/json" \
  -d '{
    "recipe": "llamacpp",
    "backend": "vulkan",
    "stream": false
  }'
```

Response format:

```json
{
  "status":"success",
  "recipe":"llamacpp",
  "backend":"vulkan"
}
```

In case of an error, returns an `error` field with details.

### Install a cloud provider
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Registers an OpenAI-compatible chat provider. The base URL is persisted to `config.json`; the optional `api_key` lives in `lemond` process memory only (cleared on restart). See the [Cloud Offload guide](../guide/configuration/cloud.md) for the full workflow.

Optional fields are applied only when present in the request body. Re-installing a provider without them keeps its stored values, so updating just the `base_url` never resets a custom auth header or the `allow_insecure_http` opt-in.

Example request:

```bash
curl -X POST http://localhost:13305/v1/install \
  -H "Content-Type: application/json" \
  -d '{
    "backend": "cloud",
    "provider": "fireworks",
    "base_url": "https://api.fireworks.ai/inference/v1"
  }'
```

Response format:

```json
{
  "status": "success",
  "backend": "cloud",
  "provider": "fireworks",
  "base_url": "https://api.fireworks.ai/inference/v1",
  "auth_header_name": "Authorization",
  "auth_header_prefix": "Bearer ",
  "wire_format": "openai",
  "models_discovered": 12,
  "auth_state": {
    "env_var_set": true,
    "runtime_key_set": false
  }
}
```

`models_discovered` is `0` when no API key is resolvable. If `api_key` is supplied but the provider's env var is also set, the response includes a `warning` string explaining the env var took precedence.

<!-- BEGIN GENERATED: lemonade.install_dry_run -->
## `POST /v1/install/dry-run`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Resolve the backend install metadata that [`POST /v1/install`](#post-v1install)
would use for a recipe/backend pair, without downloading or installing the
backend asset. Nothing is installed and no existing installation is modified.

Resolution uses the normal backend install-parameter machinery. It may consult
local configuration and, if a backend version is configured as `latest`, query
GitHub release metadata. The endpoint does not download the backend asset and
does not check whether the returned asset URL exists.

The `arch` parameter mocks ROCm GPU architecture detection while the install
parameters are resolved. This makes the endpoint useful in CI for checking
architecture-to-asset resolution on hardware that is not present on the runner.
The repository's `test/server_gfx_topology.py` uses the endpoint for this
resolution step and separately checks the resulting release URLs or
split-archive manifests.

Also served at `/api/v0/install/dry-run`, `/api/v1/install/dry-run` and `/v0/install/dry-run`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | Yes | Recipe name, for example `llamacpp`, `whispercpp`, or `vllm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Backend name within the recipe, for example `vulkan`, `rocm`, or `rocm-nightly`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `arch` | No | ROCm GPU architecture to use for this call, for example `gfx1201`. When provided, it overrides ROCm architecture detection while install parameters are resolved. When omitted, normal host detection is used; if resolution succeeds, `arch` is returned as `""` and `supported` is `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

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

### Response format

`200`

```json
{
  "recipe": "whispercpp",
  "backend": "rocm",
  "arch": "gfx1201",
  "repo": "lemonade-sdk/whisper.cpp-rocm",
  "version": "v1.8.4",
  "filename": "whisper-v1.8.4-linux-rocm-gfx120X.tar.gz",
  "url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/download/v1.8.4/whisper-v1.8.4-linux-rocm-gfx120X.tar.gz",
  "supported": true,
  "supports_split_archive": false
}
```

**Field Descriptions:**

- `recipe`, `backend`, `arch` - Echo the requested values. If `arch` was omitted, it is returned as an empty string.
- `repo`, `version`, `filename` - Install parameters produced by the backend-specific resolver. The default version pin comes from `backend_versions.json`; runtime version policy can override it.
- `url` - GitHub release-download URL constructed from `repo`, `version`, and `filename`. The endpoint does not check this URL.
- `supported` - Whether Lemonade's local recipe/backend support matrix accepts the requested `arch`. This is not a release-asset existence check. When `arch` is omitted, it is `true` on a successful response.
- `supports_split_archive` - Whether the recipe supports assets published as multiple archive parts. When `true`, the real download path can consult a `.partcount` manifest.
<!-- END GENERATED: lemonade.install_dry_run -->

A device ISA may resolve to a family target name used by the release repository.
For example, `gfx1201` resolves to the `gfx120X` family used in the Whisper
filename above. That mapping is defined by `rocm_asset_families` in
`backend_versions.json`.

An explicit architecture outside Lemonade's support matrix can still produce
install metadata; in that case `supported` is `false`. Callers that need to
verify the release asset itself must check the returned URL, or the corresponding
split-archive manifest, separately.

### Error responses

| Status | Condition |
|--------|-----------|
| `400` | `recipe` or `backend` is missing or empty. |
| `500` | The body is invalid JSON or install-parameter resolution fails, for example because the recipe/backend pair is unknown, the platform is unsupported, required architecture detection is unavailable, or version resolution fails. |

Error responses contain an `error` string. If `arch` was parsed before the
failure, the response may also include that `arch` value.

<!-- BEGIN GENERATED: lemonade.uninstall -->
## `POST /v1/uninstall`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Uninstall a backend for a specific recipe/backend pair, **or** remove a cloud provider. Dispatched by the `backend` field, mirroring `/v1/install`.

Also served at `/api/v0/uninstall`, `/api/v1/uninstall` and `/v0/uninstall`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: Recipe name | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Local backends: Backend name. Cloud providers: Must be the literal string `"cloud"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, required: Installed provider name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A local uninstall without both `recipe` and `backend`, or a cloud uninstall without `provider`, is answered with `400`. Any other failure, including invalid JSON, is answered with `500` and an `error` string.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/uninstall" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"backend": "cloud", "provider": "fireworks"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/uninstall \
      -H "Content-Type: application/json" \
      -d '{"backend": "cloud", "provider": "fireworks"}'
    ```

### Response format

`200`

```json
{"status": "success", "backend": "cloud", "provider": "fireworks", "models_evicted": 0}
```

**Field Descriptions:**

- `models_evicted` - Discovered models removed from the catalog.
<!-- END GENERATED: lemonade.uninstall -->

### Uninstall a local backend

If loaded models are using that backend, they are unloaded first.

Example request:

```bash
curl -X POST http://localhost:13305/v1/uninstall \
  -H "Content-Type: application/json" \
  -d '{
    "recipe": "llamacpp",
    "backend": "vulkan"
  }'
```

Response format:

```json
{
  "status":"success",
  "recipe":"llamacpp",
  "backend":"vulkan"
}
```

In case of an error, returns an `error` field with details.

### Uninstall a cloud provider
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Removes the provider record from `config.json`, drops its in-memory API key (if any), and evicts every discovered model for that provider from the cache. Returns 404 if the provider was never installed.

Example request:

```bash
curl -X POST http://localhost:13305/v1/uninstall \
  -H "Content-Type: application/json" \
  -d '{
    "backend": "cloud",
    "provider": "fireworks"
  }'
```

Response format:

```json
{
  "status": "success",
  "backend": "cloud",
  "provider": "fireworks",
  "models_evicted": 12
}
```

<!-- BEGIN GENERATED: lemonade.cloud_auth -->
## `POST /v1/cloud/auth`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Set an in-memory API key for a previously-installed cloud provider, and trigger a refresh of that provider's discovered model list. The key lives in `lemond` process memory only — it is never written to disk and is cleared on `lemond` restart. For persistence across restarts, set `LEMONADE_<PROVIDER>_API_KEY` in `lemond`'s environment instead.

### Authentication precedence

If `LEMONADE_<PROVIDER>_API_KEY` is set in `lemond`'s environment, the env var takes precedence and this endpoint returns **409 Conflict** without storing the supplied key. This is the safety guarantee that lets an operator provision a "house" key via env without worrying about a client silently overriding it.

Also served at `/api/v0/cloud/auth`, `/api/v1/cloud/auth` and `/v0/cloud/auth`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `provider` | Yes | Installed provider name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | Yes | API key to store in `lemond` process memory. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Opt in to sending the key to a provider whose base URL is `http://`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/cloud/auth" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"provider": "fireworks", "api_key": "fw-XXXXX"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/cloud/auth \
      -H "Content-Type: application/json" \
      -d '{"provider": "fireworks", "api_key": "fw-XXXXX"}'
    ```

### Response format

`200`

```json
{
  "provider": "fireworks",
  "allow_insecure_http": false,
  "auth_state": {"env_var_set": false, "runtime_key_set": true},
  "models_discovered": 0
}
```

**Field Descriptions:**

- `models_discovered` - Chat models discovered with the key; 0 when discovery fails.
- `warning` - The warnings joined into one string, for older clients.
<!-- END GENERATED: lemonade.cloud_auth -->

### Response format (env-var conflict — 409)

```json
{
  "error": {
    "type": "auth_conflict",
    "env_var": "LEMONADE_FIREWORKS_API_KEY",
    "message": "LEMONADE_FIREWORKS_API_KEY is set in the lemond process; the env var takes precedence and the supplied API key was not stored."
  }
}
```

### Other error responses

| Status | Cause |
|---|---|
| `400` | Body is missing `provider` or `api_key`, or one of them is empty. |
| `400` | The provider's base URL is `http://` and neither this request nor the provider's install set `allow_insecure_http` (code `insecure_http_requires_opt_in`). |
| `404` | Provider is not installed. Call `POST /v1/install` with `backend:"cloud"` first. |

<!-- BEGIN GENERATED: lemonade.cloud_auth_provider -->
## `DELETE /v1/cloud/auth/{provider}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Clear the in-memory API key for a provider. Any env-var-based key (`LEMONADE_<PROVIDER>_API_KEY`) remains in effect. If no env-var key is set, the provider's discovered models are evicted from the catalog since they are no longer authenticatable.

Also served at `/api/v0/cloud/auth/{provider}`, `/api/v1/cloud/auth/{provider}` and `/v0/cloud/auth/{provider}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `provider` (path) | Yes | Installed provider name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/cloud/auth/fireworks" `
      -Method DELETE
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/v1/cloud/auth/fireworks
    ```

### Response format

`200`

```json
{
  "provider": "fireworks",
  "cleared_runtime_key": true,
  "auth_state": {"env_var_set": false, "runtime_key_set": false}
}
```
<!-- END GENERATED: lemonade.cloud_auth_provider -->

`cleared_runtime_key` is `false` when no in-memory key was present (e.g., the only key was from the env var).

<!-- BEGIN GENERATED: lemonade.live -->
## `GET /live`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lightweight liveness probe for load balancers and orchestrators. Unlike [`/v1/health`](#get-v1health), this endpoint does no work beyond confirming the process is up — it does not inspect loaded models or backends — so it is safe to poll at high frequency. `HEAD /live` is also supported and returns `200 OK` with an empty body.

Unlike most other endpoints on this page, `/live` is not versioned and is not mounted under the `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/` prefixes.

Requires no API key.

### Parameters

This endpoint does not take any parameters.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/live"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/live
    ```

### Response format

`200`

```json
{"status": "ok"}
```
<!-- END GENERATED: lemonade.live -->

<!-- BEGIN GENERATED: lemonade.metrics -->
## `GET /metrics`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Prometheus scrape endpoint for Lemonade Server. The endpoint returns Prometheus text exposition format and is intended to be scraped by Prometheus, not by Grafana directly.

Unlike most Lemonade API endpoints, `/metrics` is root-level only. It is not mounted under `/api/v0/`, `/api/v1/`, `/v0/`, or `/v1/`.

`HEAD /metrics` is also supported and returns `200 OK` with an empty body.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Authentication

If `LEMONADE_API_KEY` is set, `/metrics` requires bearer authentication. Either the regular API key or `LEMONADE_ADMIN_API_KEY` is accepted.

If only `LEMONADE_ADMIN_API_KEY` is set and `LEMONADE_API_KEY` is unset, `/metrics` is accessible without authentication, matching regular API endpoint behavior.

### Polling and Refresh Rate

The `/metrics` endpoint has no internal refresh timer. It renders the latest server state at the moment it is scraped.

Polling frequency is configured in Prometheus via `scrape_interval`, for example:

```yaml
global:
  scrape_interval: 10s
```

Grafana queries Prometheus. Grafana's dashboard refresh controls how often panels query Prometheus, but it does not control how often Prometheus scrapes Lemonade.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/metrics"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/metrics
    ```

### Response format

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

The response uses Prometheus text exposition format:

Content type:

```text
text/plain; version=0.0.4; charset=utf-8
```

### Example request

With API-key auth:

```bash
curl http://localhost:13305/metrics \
  -H "Authorization: Bearer $LEMONADE_API_KEY"
```

### Lemonade Metric Families

The authoritative metric-family list is generated by the `/metrics` implementation in [`src/cpp/server/prometheus_metrics.cpp`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/server/prometheus_metrics.cpp). Search for `build_prometheus_metrics` and `metrics.describe(...)` to see the current names, types, labels, and descriptions.

Unsupported, unavailable, null, NaN, and infinity values are omitted rather than emitted as samples.

### llama.cpp Backend Metrics

When a loaded model uses the `llamacpp` recipe, Lemonade makes a best-effort scrape of the loaded backend process's private `/metrics` endpoint. Backend scrape failures do not fail the Lemonade `/metrics` response.

Scraped llama.cpp metrics are normalized under the `lemonade_llamacpp_*` prefix and labeled with the same Lemonade model metadata used by `lemonade_model_info`.

Lemonade starts llama.cpp backends with metrics enabled so these backend metrics are available whenever the backend supports them.

<!-- BEGIN GENERATED: lemonade.health -->
## `GET /v1/health`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Check the health of the server. This endpoint returns information about loaded models.

Also served at `/api/v0/health`, `/api/v1/health` and `/v0/health`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/health"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/health
    ```

### Response format

`200`

```json
{
  "status": "ok",
  "version": "2026.43.0~1688.60be9277",
  "model_loaded": "Qwen3-0.6B-GGUF",
  "update_check_done": true,
  "all_models_loaded": [
    {
      "model_name": "nomic-embed-text-v1-GGUF",
      "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
      "last_use": 18993468,
      "type": "embedding",
      "device": "gpu",
      "pinned": false,
      "is_busy": false,
      "is_streaming": false,
      "backend_url": "http://127.0.0.1:8003/v1",
      "pid": 94573,
      "launch_command": [
        "~/.cache/lemonade/bin/llamacpp/vulkan/llama-server",
        "-m",
        "~/.cache/huggingface/hub/models--nomic-ai--nomic-embed-text-v1-GGUF/snapshots/90c80ac5c2a0d13de9a36cf1dec39f34acee54a8/nomic-embed-text-v1.Q4_K_S.gguf",
        "--ctx-size",
        "8192",
        "--port",
        "8003",
        "--jinja",
        "--metrics",
        "--embeddings",
        "--parallel",
        "1"
      ],
      "recipe": "llamacpp",
      "recipe_options": {"ctx_size": 8192, "llamacpp_args": "--parallel 1"},
      "residency_class": "standard",
      "slot_pool": "standard/embedding",
      "status": "ready",
      "backend_alive": true,
      "backend_health": "ready",
      "loaded": true,
      "watchdog_reset": false,
      "max_context_window": 2048
    },
    {
      "model_name": "Flux-2-Klein-4B",
      "checkpoint": "black-forest-labs/FLUX.2-klein-4B:flux-2-klein-4b.safetensors",
      "last_use": 19022316,
      "type": "image",
      "device": "gpu",
      "pinned": false,
      "is_busy": false,
      "is_streaming": false,
      "backend_url": "http://127.0.0.1:8004/v1",
      "pid": 94787,
      "launch_command": [
        "~/.cache/lemonade/bin/sd-cpp/vulkan/sd-server",
        "--listen-port",
        "8004",
        "--diffusion-model",
        "~/.cache/huggingface/hub/models--black-forest-labs--FLUX.2-klein-4B/snapshots/e7b7dc27f91deacad38e78976d1f2b499d76a294/flux-2-klein-4b.safetensors",
        "--llm",
        "~/.cache/huggingface/hub/models--Comfy-Org--vae-text-encorder-for-flux-klein-4b/snapshots/a9e4ca87c16db4c4e1a16406a9ddb300ab0ae246/split_files/text_encoders/qwen_3_4b.safetensors",
        "--vae",
        "~/.cache/huggingface/hub/models--Comfy-Org--vae-text-encorder-for-flux-klein-4b/snapshots/a9e4ca87c16db4c4e1a16406a9ddb300ab0ae246/split_files/vae/flux2-vae.safetensors",
        "--vae-tiling",
        "--diffusion-fa"
      ],
      "recipe": "sd-cpp",
      "recipe_options": {
        "cfg_scale": 1.0,
        "ctx_size": 32768,
        "height": 1024,
        "sdcpp_args": "",
        "steps": 4,
        "width": 1024
      },
      "residency_class": "standard",
      "slot_pool": "standard/image",
      "status": "ready",
      "backend_alive": true,
      "backend_health": "ready",
      "loaded": true,
      "watchdog_reset": false
    },
    {
      "model_name": "Qwen3-0.6B-GGUF",
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "last_use": 19053963,
      "type": "llm",
      "device": "gpu",
      "pinned": false,
      "is_busy": false,
      "is_streaming": false,
      "backend_url": "http://127.0.0.1:8005/v1",
      "pid": 95898,
      "launch_command": ["~/.cache/lemonade/bin/llamacpp/vulkan/llama-server", "-m", "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "--ctx-size", "40960", "--port", "8005", "--jinja", ... 13 more],
      "recipe": "llamacpp",
      "recipe_options": {
        "ctx_size": 40960,
        "llamacpp_args": "--temp 0.6 --top-p 0.85 --top-k 20 --min-p 0.0 --repeat-penalty 1.0 --parallel 1"
      },
      "residency_class": "standard",
      "slot_pool": "standard/llm",
      "status": "ready",
      "backend_alive": true,
      "backend_health": "ready",
      "loaded": true,
      "watchdog_reset": false,
      "max_context_window": 40960
    }
  ],
  "pinned_models": {
    "classification": 0,
    "embedding": 0,
    "image": 0,
    "llm": 0,
    "reranking": 0,
    "transcription": 0,
    "tts": 0
  },
  "max_models": {
    "llm": 1,
    "embedding": 1,
    "reranking": 1,
    "transcription": 1,
    "image": 1,
    "tts": 1,
    "classification": 1
  },
  "websocket_port": 9001,
  "telemetry": {"enabled": false},
  "pinned_helper_models": {
    "classification": 0,
    "embedding": 0,
    "image": 0,
    "llm": 0,
    "reranking": 0,
    "transcription": 0,
    "tts": 0
  }
}
```

**Field Descriptions:**

- `status` - Server health status, always `"ok"`
- `version` - Version number of Lemonade Server
- `model_loaded` - Model name of the most recently accessed model
- `update_check_done` - Whether the background HuggingFace model update check has completed at startup. Poll this field after server start to know when `update_available` fields are ready.
- `all_models_loaded` - Array of all currently loaded models with details:
  - `model_name` - Name of the loaded model
  - `checkpoint` - Full checkpoint identifier
  - `last_use` - Monotonic timestamp of last access (load or inference), in milliseconds
  - `type` - Model type: `"llm"`, `"embedding"`, `"reranking"`, `"transcription"`, `"image"`, `"tts"`, `"audio-generation"`, `"classification"`, or `"mesh"`
  - `device` - Space-separated device list: `"cpu"`, `"gpu"`, `"npu"`, or combinations like `"gpu npu"`
  - `pinned` - Boolean indicating if the model is currently pinned to prevent auto-eviction
  - `is_busy` - Boolean indicating if the model has active requests or maintenance in progress
  - `is_streaming` - Boolean indicating if the model is actively generating output tokens (true after first chunk arrives, false when all streaming requests complete)
  - `backend_url` - URL of the backend server process handling this model (useful for debugging)
  - `pid` - The Process ID (PID) of the backend engine handling this model
  - `launch_command` - *(optional)* The command used to start the backend engine, as an array with the program first and its arguments after it. Every local backend has one. Cloud models don't, because they don't start a program. The values shown are the ones actually used, so a `ctx_size` of `auto` appears here as a real number, and any flags Lemonade added on its own are included.
  - `recipe` - Backend/device recipe used to load the model (e.g., `"ryzenai-llm"`, `"llamacpp"`, `"flm"`)
  - `recipe_options` - Options used to load the model (e.g., `"ctx_size"`, `"llamacpp_backend"`, `"llamacpp_args"`, `"whispercpp_args"`)
  - `residency_class` - standard, or routing_helper for a model a router policy keeps resident.
  - `slot_pool` - The loaded-model limit the model counts against, or unmetered.
  - `watchdog_reset` - Whether the backend watchdog has reset this backend.
- `pinned_models` - Counts of pinned models currently loaded in memory per model type (e.g., `llm`, `embedding`, etc.)
- `max_models` - Maximum number of models that can be loaded simultaneously per type (set via `max_loaded_models` in [Server Configuration](../guide/configuration/README.md)):
  - `llm` - Maximum LLM/chat models
  - `embedding` - Maximum embedding models
  - `reranking` - Maximum reranking models
  - `transcription` - Maximum speech-to-text models
  - `image` - Maximum image models
  - `tts` - Maximum text-to-speech models
- `websocket_port` - *(optional)* Port of the WebSocket server for the [Realtime Audio Transcription API](./openai.md#ws-realtime) and [Log Streaming API](#log-streaming-api-websocket). Only present when the WebSocket server is running. The port is OS-assigned or set via `--websocket-port`. The main port serves both APIs too.
- `telemetry` - Structured telemetry state object:
  - `enabled` - Boolean indicating if telemetry collection is active
  - `captures` - *(optional)* Array of captured telemetry components (e.g., `["inputs", "outputs", "thinking"]`), only present when `enabled` is `true`.
- `pinned_helper_models` - Pinned routing-helper models of each type.
<!-- END GENERATED: lemonade.health -->

<!-- BEGIN GENERATED: lemonade.docs -->
## `GET /v1/docs`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

List the API reference pages bundled with the server. The documentation ships with the
server, so it describes the version you are actually running and requires no internet
access.

Fetch this index first, then read the pages it advertises. New pages can be added in
future releases without breaking clients, because every entry carries its own URL.

Also served at `/api/v0/docs`, `/api/v1/docs` and `/v0/docs`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/docs"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/docs
    ```

### Response format

`200`

```json
{
  "version": "2026.43.0~1686.1553ede6",
  "format": "text/markdown",
  "docs": [
    {
      "id": "api/README",
      "title": "Lemonade Endpoints Spec",
      "url": "/v1/docs/api/README",
      "bytes": 1778
    },
    {
      "id": "api/anthropic",
      "title": "Anthropic-Compatible API",
      "url": "/v1/docs/api/anthropic",
      "bytes": 893
    },
    {
      "id": "api/internal",
      "title": "Internal API",
      "url": "/v1/docs/api/internal",
      "bytes": 453
    },
    {
      "id": "api/lemonade",
      "title": "Lemonade API",
      "url": "/v1/docs/api/lemonade",
      "bytes": 99280
    },
    {
      "id": "api/llamacpp",
      "title": "llama.cpp-Specific API",
      "url": "/v1/docs/api/llamacpp",
      "bytes": 14841
    },
    {"id": "api/mcp", "title": "MCP Gateway", "url": "/v1/docs/api/mcp", "bytes": 18109},
    {
      "id": "api/ollama",
      "title": "Ollama-Compatible API",
      "url": "/v1/docs/api/ollama",
      "bytes": 1180
    },
    {
      "id": "api/openai",
      "title": "OpenAI-Compatible API",
      "url": "/v1/docs/api/openai",
      "bytes": 55451
    },
    {"id": "api/router", "title": "Router API", "url": "/v1/docs/api/router", "bytes": 2967}
  ]
}
```

**Field Descriptions:**

- `version` - Lemonade Server version the pages describe.
- `docs`
  - `id` - Page id, mirroring the page's path on the documentation website.
  - `url` - Where to read the page.
<!-- END GENERATED: lemonade.docs -->

`url` is returned with the same prefix used to request the index, so a client that queries
`/api/v0/docs` receives `/api/v0/docs/...` URLs.

<!-- BEGIN GENERATED: lemonade.docs_page -->
## `GET /v1/docs/{page}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Read one page, served as `Content-Type: text/markdown`. `{page}` is the `id` from the index,
which mirrors the path used on the documentation website; the `.md` suffix is optional.

Also served at `/api/v0/docs/{page}`, `/api/v1/docs/{page}` and `/v0/docs/{page}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `page` (path) | Yes | The `id` from the [`GET /v1/docs`](#get-v1docs) index, which mirrors the page's path on the documentation website. The `.md` suffix is optional. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/docs/api/lemonade"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/docs/api/lemonade
    ```

### Response format

`200`

```text
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
...
```
<!-- END GENERATED: lemonade.docs_page -->

Unknown pages return `404`.

### Reading the files directly

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

Performance statistics from the last request.

Also served at `/api/v0/stats`, `/api/v1/stats` and `/v0/stats`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/stats"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/stats
    ```

### Response format

`200`

```json
{
  "time_to_first_token": 0.003227,
  "tokens_per_second": 336.57753121020744,
  "input_tokens": 1,
  "output_tokens": 264,
  "prompt_tokens": 15,
  "cache_tokens": 14,
  "request_count_total": 8,
  "input_tokens_total": 83,
  "output_tokens_total": 1300,
  "prompt_tokens_total": 144,
  "cache_tokens_total": 75,
  "routing_decisions_total": 0,
  "routing_switches_total": 0
}
```

**Field Descriptions:**

- `time_to_first_token` - Time in seconds until the first token was generated
- `tokens_per_second` - Generation speed in tokens per second
- `input_tokens` - Number of tokens processed
- `output_tokens` - Number of tokens generated
- `prompt_tokens` - Total prompt tokens including cached tokens
- `cache_tokens` - Prompt tokens served from the backend's prefix cache on the last request (llama.cpp `timings.cache_n`, or `usage.prompt_tokens_details.cached_tokens` / Responses-API `input_tokens_details.cached_tokens` from OpenAI-compatible cloud providers). `null` when the last request did not report cache usage
- `request_count_total`, `input_tokens_total`, `output_tokens_total`, `prompt_tokens_total`, `cache_tokens_total` - Cumulative counters since server start
- `routing_decisions_total` - Routing decisions made by `collection.router` dispatch
- `routing_switches_total` - Routing decisions that changed a conversation's routed model (a proxy for route ping-pong; conversations are identified by a hash of the system prompt and first user message)
<!-- END GENERATED: lemonade.stats -->

<!-- BEGIN GENERATED: lemonade.system_stats -->
## `GET /v1/system-stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Current host resource usage as measured by the Lemonade Server process. This endpoint is useful for first-party clients and dashboards that need lightweight runtime telemetry without scraping Prometheus.

Also served at `/api/v0/system-stats`, `/api/v1/system-stats` and `/v0/system-stats`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/system-stats"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/system-stats
    ```

### Response format

`200`

```json
{
  "cpu_percent": 0.0,
  "memory_gb": 33.0,
  "gpu_percent": 71.0,
  "vram_gb": 21.594070434570312,
  "primary_gpu_id": "amd:0000:c5:00.0",
  "gpus": [
    {
      "id": "amd:0000:c5:00.0",
      "vendor": "amd",
      "name": "Radeon 8060S Graphics",
      "memory_type": "shared",
      "gpu_percent": 71.0,
      "memory_total_gb": 61.845550537109375,
      "memory_used_gb": 21.594070434570312
    }
  ],
  "npu_percent": 0.0
}
```

**Field Descriptions:**

- `cpu_percent` - System CPU utilization percentage, or `null` when unavailable
- `memory_gb` - System RAM currently in use, in GiB
- `gpu_percent` - GPU utilization percentage, or `null` when unavailable
- `vram_gb` - GPU memory currently in use, in GiB, or `null` when unavailable
- `primary_gpu_id` - `id` of the GPU that `gpu_percent` and `vram_gb` describe, or `null` when no GPU is enumerated
- `gpus` - Every enumerated host GPU, primary first
  - `id` - Stable device ID, e.g. `amd:0000:c1:00.0`
  - `vendor` - `amd` or `nvidia`
  - `name` - Device name
  - `memory_type` - `dedicated`, `shared`, or `unknown`
  - `gpu_percent` - GPU utilization percentage, or `null` when unavailable
  - `memory_total_gb` - GPU-accessible memory capacity, in GiB, or `null` when unavailable
  - `memory_used_gb` - GPU memory currently in use, in GiB, or `null` when unavailable
- `npu_percent` - NPU utilization percentage, or `null` when unavailable
<!-- END GENERATED: lemonade.system_stats -->

GPU, VRAM, and NPU telemetry availability depends on the operating system and installed drivers. Unsupported values are returned as `null`.

The `primary_gpu_id` and `gpus` fields describe the individual host GPUs. The legacy
`gpu_percent` and `vram_gb` values now come from the same primary device: the GPU
with the largest known GPU-accessible memory capacity (ties broken by stable ID).
Selection does not depend on current GPU utilization. Device IDs remain stable
across ordinary telemetry failures; unavailable live measurements become `null`.
The primary GPU for this display is **not** necessarily the GPU targeted by a
model recipe. `vram_gb` retains its historical name but represents GPU memory
usage, which can be shared system memory for AMD APUs.

Example `gpus` entry (all measurement fields can be `null`):

```json
{
  "primary_gpu_id": "amd:0000:c1:00.0",
  "gpus": [
    {
      "id": "amd:0000:c1:00.0",
      "vendor": "amd",
      "name": "AMD GPU (card0)",
      "memory_type": "shared",
      "gpu_percent": 25.0,
      "memory_total_gb": 120.0,
      "memory_used_gb": 18.0
    }
  ]
}
```

On Linux, AMD and NVIDIA devices are enumerated separately. On Windows this
change provides NVIDIA NVML telemetry; other vendors still require their own
platform implementation. On macOS the new per-GPU list is empty.
NVML alone does not establish whether memory is dedicated or shared, so
NVIDIA devices report `memory_type` as `unknown`.

<!-- BEGIN GENERATED: lemonade.system_info -->
## `GET /v1/system-info`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

System information endpoint that provides complete hardware details and device enumeration.

Also served at `/api/v0/system-info`, `/api/v1/system-info` and `/v0/system-info`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/system-info"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/system-info
    ```

### Response format

`200`

```json
{
  "OS Version": "Linux-7.0.0-34-generic (Ubuntu 26.04)",
  "Processor": "AMD RYZEN AI MAX+ 395 w/ Radeon 8060S",
  "Physical Memory": "122.69 GB",
  "model_storage": {
    "path": "~/.cache/huggingface/hub",
    "used_bytes": 1399147024384,
    "total_bytes": 2013755310080,
    "free_bytes": 614608285696
  },
  "devices": {
    "cpu": {
      "available": true,
      "cores": 16,
      "family": "x86_64",
      "name": "AMD RYZEN AI MAX+ 395 w/ Radeon 8060S",
      "threads": 32
    },
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
    "nvidia_gpu": [{"available": false, "error": "No NVIDIA discrete GPU found", "name": ""}],
    "amd_npu": {
      "available": true,
      "family": "XDNA2",
      "name": "AMD RYZEN AI MAX+ 395 w/ Radeon 8060S",
      "power_mode": "DEFAULT",
      "tops_max_int": 58,
      "utilization": 0.0
    }
  },
  "recipes": {
    "acestep": {
      "default_backend": "vulkan",
      "experimental": true,
      "backends": {
        "cuda": {
          "devices": [],
          "state": "unsupported",
          "message": "Unsupported GPU",
          "action": "",
          "version": "v0.1.1",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/acestep.cpp/releases/tag/v0.1.1",
          "download_filename": "acestep-cuda-linux-x64.tar.gz"
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install acestep:rocm",
          "version": "v0.1.1",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/acestep.cpp/releases/tag/v0.1.1",
          "download_filename": "acestep-rocm-linux-x64.tar.gz"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v0.1.1",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/acestep.cpp/releases/tag/v0.1.1",
          "download_filename": "acestep-vulkan-linux-x64.tar.gz"
        }
      },
      "display_name": "ACE-Step",
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        },
        {
          "backend": "vulkan",
          "device_summary": "Vulkan-capable GPUs",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["x86_64"]},
            {"device": "nvidia_gpu", "families": []}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "ACE-Step"
    },
    "ds4": {
      "default_backend": "rocm",
      "experimental": true,
      "backends": {
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install ds4:rocm",
          "version": "b0001",
          "tier": "guest",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/ds4-rocm/releases/tag/b0001",
          "download_filename": "ds4-b0001-linux-rocm-gfx1151-x64.tar.gz"
        }
      },
      "display_name": "DwarfStar4",
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
          "format": "native",
          "os": ["linux"],
          "tier": "guest"
        }
      ],
      "uses_ctx_size": true,
      "web_display_name": "DwarfStar4"
    },
    "flm": {
      "default_backend": "npu",
      "experimental": false,
      "backends": {
        "npu": {
          "devices": ["amd_npu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install flm:npu",
          "version": "v1.0.5",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/ROCm/FastFlowLM/releases/tag/v1.0.7",
          "download_filename": "fastflowlm_1.0.7_linux.tar.gz"
        }
      },
      "display_name": "FastFlowLM NPU",
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": true,
      "web_display_name": "FastFlowLM NPU"
    },
    "kokoro": {
      "default_backend": "cpu",
      "experimental": false,
      "backends": {
        "cpu": {
          "devices": ["cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "b21",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/Kokoros/releases/tag/b21",
          "download_filename": "kokoros-linux-x86_64.tar.gz"
        },
        "metal": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires macOS",
          "action": "",
          "version": "b21",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/Kokoros/releases/tag/b21",
          "can_uninstall": true
        }
      },
      "display_name": "Kokoro",
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
          "format": "native",
          "os": ["macos"],
          "tier": "core"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64 CPU",
          "devices": [{"device": "cpu", "families": ["x86_64"]}],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "Kokoro"
    },
    "llamacpp": {
      "default_backend": "vulkan",
      "experimental": false,
      "backends": {
        "cpu": {
          "devices": ["cpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install llamacpp:cpu",
          "version": "b11393",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/ggml-org/llama.cpp/releases/tag/b11393",
          "download_filename": "llama-b11393-bin-ubuntu-x64.tar.gz"
        },
        "cuda": {
          "devices": [],
          "state": "unsupported",
          "message": "Unsupported GPU",
          "action": "",
          "tier": "core",
          "format": "native"
        },
        "metal": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires macOS",
          "action": "",
          "tier": "core",
          "format": "native",
          "can_uninstall": true
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install llamacpp:rocm",
          "version": "b10820",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/llama.cpp/releases/tag/b11395",
          "download_filename": "llama-b11395-bin-ubuntu-rocm-10.0-x64.tar.gz"
        },
        "system": {
          "devices": ["cpu"],
          "state": "unsupported",
          "message": "llama-server not found in PATH",
          "action": "",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com//releases/tag/"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "b11393",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/ggml-org/llama.cpp/releases/tag/b11393",
          "download_filename": "llama-b11393-bin-ubuntu-vulkan-x64.tar.gz"
        }
      },
      "display_name": "Llama.cpp GPU",
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
          "format": "native",
          "os": ["linux"],
          "tier": "core"
        },
        {
          "backend": "metal",
          "device_summary": "Apple Silicon GPU",
          "devices": [{"device": "metal", "families": []}],
          "format": "native",
          "os": ["macos"],
          "tier": "core"
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        },
        {
          "backend": "vulkan",
          "device_summary": "x86_64 CPU, AMD iGPU, AMD dGPU; ARM64 CPU/GPU (Linux)",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["arm64", "x86_64"]}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64 CPU; ARM64 CPU (Linux)",
          "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": true,
      "web_display_name": "llama.cpp GPU"
    },
    "llamacpp-hrx": {
      "default_backend": "hrx",
      "experimental": true,
      "backends": {
        "hrx": {
          "devices": ["amd_gpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install llamacpp-hrx:hrx",
          "version": "hrx-b59",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/ROCm/ggml-staging-automation/releases/tag/hrx-b99",
          "download_filename": "llama-hrx-b99-bin-manylinux-hrx-x64.tar.gz"
        }
      },
      "display_name": "HRX GPU",
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
          "format": "native",
          "os": ["linux"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": true,
      "web_display_name": "HRX GPU"
    },
    "moonshine": {
      "default_backend": "cpu",
      "experimental": false,
      "backends": {
        "cpu": {
          "devices": ["cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "moonshine0.0.62",
          "tier": "core",
          "format": "python",
          "release_url": "https://github.com/lemonade-sdk/moonshine-server-rocm/releases/tag/moonshine0.0.62",
          "download_filename": "moonshine-server-moonshine0.0.62-linux-x64.tar.gz"
        }
      },
      "display_name": "Moonshine",
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
          "format": "python",
          "os": ["windows"],
          "tier": "core"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64/arm64 CPU",
          "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
          "format": "python",
          "os": ["linux"],
          "tier": "core"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64/arm64 CPU",
          "devices": [{"device": "cpu", "families": ["arm64"]}],
          "format": "python",
          "os": ["macos"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "Moonshine"
    },
    "onnxruntime": {
      "default_backend": "cpu",
      "experimental": true,
      "backends": {
        "cpu": {
          "devices": ["cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "0.3.7",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/ort-server/releases/tag/0.3.7",
          "download_filename": "ort-server-0.3.7-linux-x64.tar.gz"
        }
      },
      "display_name": "ONNX Runtime",
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
          "format": "native",
          "os": ["windows"],
          "tier": "experimental"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64/arm64 CPU",
          "devices": [{"device": "cpu", "families": ["arm64", "x86_64"]}],
          "format": "native",
          "os": ["linux"],
          "tier": "experimental"
        },
        {
          "backend": "cpu",
          "device_summary": "arm64 CPU",
          "devices": [{"device": "cpu", "families": ["arm64"]}],
          "format": "native",
          "os": ["macos"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "ONNX Runtime"
    },
    "openmoss": {
      "default_backend": "vulkan",
      "experimental": true,
      "backends": {
        "cuda": {
          "devices": [],
          "state": "unsupported",
          "message": "Unsupported GPU",
          "action": "",
          "version": "v0.3.0",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/openmoss/releases/tag/v0.3.0",
          "download_filename": "moss-tts-cuda-linux-x64.tar.gz"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v0.3.0",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/openmoss/releases/tag/v0.3.0",
          "download_filename": "moss-tts-vulkan-linux-x64.tar.gz"
        }
      },
      "display_name": "OpenMOSS TTS",
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        },
        {
          "backend": "vulkan",
          "device_summary": "Vulkan-capable GPUs",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["x86_64"]},
            {"device": "nvidia_gpu", "families": []}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "OpenMOSS TTS"
    },
    "ryzenai-llm": {
      "experimental": false,
      "backends": {
        "npu": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires Windows",
          "action": "",
          "version": "v1.7.0",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/ryzenai-server/releases/tag/v1.7.0",
          "download_filename": "ryzenai-server.zip",
          "can_uninstall": true
        }
      },
      "display_name": "Ryzen AI LLM",
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
          "format": "native",
          "os": ["windows"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": true,
      "web_display_name": "Ryzen AI SW NPU"
    },
    "sd-cpp": {
      "default_backend": "vulkan",
      "experimental": false,
      "backends": {
        "cpu": {
          "devices": ["cpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install sd-cpp:cpu",
          "version": "master-721-8caa3f9",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
          "download_filename": "sd-master-017cc8e-bin-Linux-Ubuntu-24.04-x86_64.zip"
        },
        "cuda": {
          "devices": [],
          "state": "unsupported",
          "message": "Unsupported GPU",
          "action": "",
          "tier": "core",
          "format": "native"
        },
        "metal": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires macOS",
          "action": "",
          "version": "master-894-017cc8e",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
          "can_uninstall": true
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install sd-cpp:rocm",
          "version": "master-843-462d675",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
          "download_filename": "sd-master-017cc8e-bin-Linux-Ubuntu-24.04-x86_64-rocm-10.0.0.zip"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "master-894-017cc8e",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/stable-diffusion.cpp/releases/tag/master-894-017cc8e",
          "download_filename": "sd-master-017cc8e-bin-Linux-Ubuntu-24.04-x86_64-vulkan.zip"
        }
      },
      "display_name": "StableDiffusion.cpp",
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
          "format": "native",
          "os": ["macos"],
          "tier": "core"
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        },
        {
          "backend": "vulkan",
          "device_summary": "Vulkan-capable GPUs",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["x86_64"]},
            {"device": "nvidia_gpu", "families": []}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
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
          "format": "native",
          "os": ["linux"],
          "tier": "core"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64 CPU",
          "devices": [{"device": "cpu", "families": ["x86_64"]}],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "stable-diffusion.cpp"
    },
    "thenoise": {
      "default_backend": "rocm",
      "experimental": true,
      "backends": {
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install thenoise:rocm",
          "version": "thenoise-0.9.1-rocm10.1.0",
          "tier": "experimental",
          "format": "python",
          "release_url": "https://github.com/lemonade-sdk/thenoise/releases/tag/thenoise-0.10.1-rocm10.1.0",
          "download_filename": "thenoise-0.10.1-rocm10.1.0-gfx1151-x64.tar.gz"
        }
      },
      "display_name": "TheNoise ROCm",
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
          "format": "python",
          "os": ["linux", "windows"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "thenoise"
    },
    "thinksound": {
      "default_backend": "vulkan",
      "experimental": true,
      "backends": {
        "cuda": {
          "devices": [],
          "state": "unsupported",
          "message": "Unsupported GPU",
          "action": "",
          "version": "v0.1.2",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/thinksound.cpp/releases/tag/v0.1.2",
          "download_filename": "thinksound-cuda-linux-x64.tar.gz"
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install thinksound:rocm",
          "version": "v0.1.2",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/thinksound.cpp/releases/tag/v0.1.2",
          "download_filename": "thinksound-rocm-linux-x64.tar.gz"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v0.1.2",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/thinksound.cpp/releases/tag/v0.1.2",
          "download_filename": "thinksound-vulkan-linux-x64.tar.gz"
        }
      },
      "display_name": "ThinkSound",
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        },
        {
          "backend": "vulkan",
          "device_summary": "Vulkan-capable GPUs",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["x86_64"]},
            {"device": "nvidia_gpu", "families": []}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "ThinkSound"
    },
    "trellis": {
      "default_backend": "vulkan",
      "experimental": true,
      "backends": {
        "cuda": {
          "devices": [],
          "state": "unsupported",
          "message": "Unsupported GPU",
          "action": "",
          "version": "v0.4.3",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/trellis.cpp/releases/tag/v0.4.3",
          "download_filename": "trellis-cuda-linux-x64.tar.gz"
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v0.4.3",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/trellis.cpp/releases/tag/v0.4.3",
          "download_filename": "trellis-rocm-linux-x64.tar.gz"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v0.4.3",
          "tier": "experimental",
          "format": "native",
          "release_url": "https://github.com/pwilkin/trellis.cpp/releases/tag/v0.4.3",
          "download_filename": "trellis-vulkan-linux-x64.tar.gz"
        }
      },
      "display_name": "TRELLIS.2",
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        },
        {
          "backend": "vulkan",
          "device_summary": "Vulkan-capable GPUs",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["x86_64"]},
            {"device": "nvidia_gpu", "families": []}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
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
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "TRELLIS.2"
    },
    "vllm": {
      "default_backend": "rocm",
      "experimental": true,
      "backends": {
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "vllm0.20.1-rocm7.12.0-gfx1151",
          "tier": "experimental",
          "format": "python",
          "release_url": "https://github.com/lemonade-sdk/vllm-rocm/releases/tag/vllm0.20.1-rocm7.12.0-gfx1151",
          "download_filename": "vllm0.20.1-rocm7.12.0-gfx1151-x64.tar.gz"
        }
      },
      "display_name": "vLLM ROCm",
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
          "format": "python",
          "os": ["linux"],
          "tier": "experimental"
        }
      ],
      "uses_ctx_size": true,
      "web_display_name": "vLLM ROCm"
    },
    "whispercpp": {
      "default_backend": "vulkan",
      "experimental": false,
      "backends": {
        "cpu": {
          "devices": ["cpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install whispercpp:cpu",
          "version": "v1.8.4",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
          "download_filename": "whisper-v1.8.4-linux-cpu-x86_64.tar.gz"
        },
        "metal": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires macOS",
          "action": "",
          "version": "v1.8.4",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
          "download_filename": "whisper-v1.8.4-darwin-metal-arm64.tar.gz",
          "can_uninstall": true
        },
        "npu": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires Windows",
          "action": "",
          "tier": "core",
          "format": "native",
          "can_uninstall": true
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v1.8.4",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
          "download_filename": "whisper-v1.8.4-linux-rocm-gfx1151.tar.gz"
        },
        "vulkan": {
          "devices": ["amd_gpu", "cpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "v1.8.4",
          "tier": "core",
          "format": "native",
          "release_url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/tag/v1.8.4",
          "download_filename": "whisper-v1.8.4-linux-vulkan-x86_64.tar.gz"
        }
      },
      "display_name": "Whisper.cpp",
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
          "format": "native",
          "os": ["windows"],
          "tier": "core"
        },
        {
          "backend": "metal",
          "device_summary": "Apple Silicon GPU",
          "devices": [{"device": "metal", "families": []}],
          "format": "native",
          "os": ["macos"],
          "tier": "core"
        },
        {
          "backend": "vulkan",
          "device_summary": "x86_64 CPU",
          "devices": [
            {"device": "amd_gpu", "families": []},
            {"device": "cpu", "families": ["x86_64"]}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        },
        {
          "backend": "rocm",
          "device_summary": "Supported AMD ROCm iGPU/dGPU families*",
          "devices": [
            {"device": "amd_gpu", "families": ["gfx110X", "gfx1150", "gfx1151", "gfx120X"]}
          ],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        },
        {
          "backend": "cpu",
          "device_summary": "x86_64 CPU",
          "devices": [{"device": "cpu", "families": ["x86_64"]}],
          "format": "native",
          "os": ["linux", "windows"],
          "tier": "core"
        }
      ],
      "uses_ctx_size": false,
      "web_display_name": "whisper.cpp"
    }
  },
  "cloud": {"providers": []},
  "unavailable_recipes": [],
  "no_fetch_executables": false
}
```

**Field Descriptions:**

- `OS Version` - Operating system name and version
- `Processor` - CPU model name
- `Physical Memory` - Total RAM
- `OEM System` - System/laptop model name (Windows only)
- `BIOS Version` - BIOS information (Windows only)
- `CPU Max Clock` - Maximum CPU clock speed (Windows only)
- `Windows Power Setting` - Current power plan (Windows only)
- `model_storage` - Drive-level storage information for the active configured model storage path. Values are reported in bytes for storage meters; this is not a recursive sum of Lemonade model files.
  - `path` - Active model storage path from server configuration
  - `used_bytes` - Used bytes on the model-storage drive
  - `total_bytes` - Total capacity of the model-storage drive
  - `free_bytes` - Free bytes available to the Lemonade Server process on the model-storage drive
  - `error` - Only when the drive could not be measured.
- `devices` - Hardware devices detected on the system (no software/support information)
  - `cpu` - CPU information (name, cores, threads)
  - `amd_gpu` - Array of AMD GPUs, both integrated and discrete (if present)
  - `nvidia_gpu` - Array of NVIDIA GPUs (if present)
  - `amd_npu` - AMD NPU device (if present)
- `recipes` - Software recipes and their backend support status
  - Each recipe (e.g., `llamacpp`, `whispercpp`, `flm`) contains:
    - `default_backend` - Preferred backend selected by server policy for this system (present when at least one backend is not `unsupported`)
    - `experimental` - `true` when the `default_backend`'s tier is `experimental` or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
    - `backends` - Available backends for this recipe
      - Each backend contains:
        - `devices` - List of devices **on this system** that support this backend (empty if not supported)
        - `state` - Backend lifecycle state: `unsupported`, `installable`, `update_required`, or `installed`
        - `message` - Human-readable status text for GUI and CLI users. Required for `unsupported`, `installable`, and `update_required`; empty for `installed`.
        - `action` - Actionable user instruction string. For install/update cases this is typically an exact CLI command; for other states it may be empty or another actionable value (for example, a URL).
        - `version` - Installed or configured backend version (when available)
        - `tier` - `core`, `experimental`, or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
        - `format` - `native`, `python`, or `container`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
- `cloud` - Cloud OpenAI-compatible providers configured on this server. Contains:
  - `providers` - Array, one entry per installed provider:
    - `name` - Provider name used as the model-name prefix (e.g. `fireworks`).
    - `base_url` - Persisted base URL from `config.json`.
    - `auth_header_name` - Header this provider's API key is sent in (default `Authorization`).
    - `auth_header_prefix` - Value prefix placed before the key (default `Bearer `).
    - `wire_format` - Request/response shape this provider speaks: `openai` (default) or `anthropic`.
    - `env_var` - Canonical environment variable name for this provider's API key (e.g. `LEMONADE_FIREWORKS_API_KEY`). The variable's *name* is reported, never its value.
    - `env_var_set` - `true` if the env var is set in `lemond`'s environment.
    - `runtime_key_set` - `true` if an in-memory key has been supplied via `POST /v1/cloud/auth` this session.
    - `models_discovered` - Number of chat-capable models currently in the catalog for this provider.
- `unavailable_recipes` - Recipes all of whose models are filtered out on this host. Reported separately so recipes stays the same on every host; dynamic-model backends (cloud, flm) are never listed.
- `no_fetch_executables` - Whether the server is configured not to download backend executables.
<!-- END GENERATED: lemonade.system_info -->

### Backend Tiers and Formats

| Tier | Meaning |
|------|---------|
| `core` | Supported by Lemonade's maintainers for production use. |
| `experimental` | Provided by Lemonade's maintainers, not supported for production use. |
| `guest` | Developed and published outside Lemonade's maintainers. Use at your own risk. The models of a recipe whose only backends on this system are guest backends stay hidden from `GET /v1/models?show_all=true` until one of those backends is installed. |

| Format | Meaning |
|--------|---------|
| `native` | A compiled executable. |
| `python` | Python, packaged with its own interpreter. |
| `container` | An OCI image, run by a pre-installed Podman or Docker. |

The same backend can carry different labels on different operating systems. Each entry in a recipe's `backends` object reports the labels for this system's operating system.

## Log Streaming API (WebSocket)
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Stream server logs over WebSocket. Clients connect, send a subscribe message, and receive a snapshot of recent log history followed by live log entries as they occur.

### Connection

The WebSocket server shares the same port as the [Realtime Audio Transcription API](./openai.md#ws-realtime). Discover the port via the [`/v1/health`](#get-v1health) endpoint (`websocket_port` field), then connect:

```
ws://localhost:<websocket_port>/logs/stream
```

The main HTTP port also accepts `/logs/stream` connections, for example `ws://localhost:13305/logs/stream`.

After connecting, send a `logs.subscribe` message to start receiving logs.

### Client → Server Messages

| Message Type | Description |
|--------------|-------------|
| `logs.subscribe` | Subscribe to log stream. Optional `after_seq` field to resume from a specific sequence number. |

### Server → Client Messages

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

## Job Engine API

<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Run client-posted sequences of server operations as durable, background **jobs** — steps that pass data forward, branch on results, and have a pause / interrupt / resume / delete / query lifecycle that survives client disconnect and server restart. Exclusive ops (`load`/`unload`/`chat`) hold a Router slot so normal traffic queues behind a running job.

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/v1/jobs` | Create a job from `{name, definition:{steps} \| steps, inputs}`; returns `202 {"id"}`, or `400` on an invalid step graph. |
| `GET` | `/v1/jobs` | List job summaries. |
| `GET` | `/v1/jobs/{id}` | Full job record (status, per-step state, context). |
| `POST` | `/v1/jobs/{id}/pause` | Stop after the current step. |
| `POST` | `/v1/jobs/{id}/interrupt` | Cancel the current step now; resumable. |
| `POST` | `/v1/jobs/{id}/resume` | Continue a paused/interrupted job. |
| `DELETE` | `/v1/jobs/{id}` | Remove a job. |

See [`docs/dev/job-system.md`](../dev/job-system.md) for the step schema, op set, and lifecycle, and [`docs/dev/job-expression-language.md`](../dev/job-expression-language.md) for the `when`/`branch` expression grammar.

<!-- BEGIN GENERATED: lemonade.jobs_create -->
## `POST /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Creates a job: a sequence of server operations that runs in the background, passes data between steps and branches on results, surviving client disconnects and server restarts. See [Job Engine API](#job-engine-api).

Also served at `/api/v0/jobs`, `/api/v1/jobs` and `/v0/jobs`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `name` | No | Display name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | The steps, in order; see [Recipe: steps](../dev/job-system.md#recipe-steps). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `definition` | No | Alternative to `steps`: an object whose `steps` holds the steps. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `inputs` | No | Values the steps read as `context.inputs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The step graph is validated at creation; an invalid one is answered with `400`. When the job store holds 50 jobs that are all still active or resumable, the answer is `429` until one is deleted or finishes.

### Example request

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

### Response format

`202`

```json
{"id": "job-20261007-171503-000001"}
```

**Field Descriptions:**

- `id` - The job's id.
<!-- END GENERATED: lemonade.jobs_create -->

<!-- BEGIN GENERATED: lemonade.jobs_list -->
## `GET /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Lists a summary of every job, active or finished.

Also served at `/api/v0/jobs`, `/api/v1/jobs` and `/v0/jobs`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/jobs"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs
    ```

### Response format

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

**Field Descriptions:**

- `jobs`
  - `progress`
    - `cursor` - Id of the current or next step.
    - `completed` - Steps that need no more work: completed, skipped, or failed with the failure handled.
<!-- END GENERATED: lemonade.jobs_list -->

<!-- BEGIN GENERATED: lemonade.jobs_pause -->
## `POST /v1/jobs/{id}/pause`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Stops a job after its current step and releases the model slot, so queued requests run. A queued job pauses at once. Models the job loaded stay loaded for the resume.

Also served at `/api/v0/jobs/{id}/pause`, `/api/v1/jobs/{id}/pause` and `/v0/jobs/{id}/pause`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job, or one that is not queued or running, is answered with `404`.

### Example request

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

### Response format

`200`

```json
{"status": "pausing"}
```
<!-- END GENERATED: lemonade.jobs_pause -->

<!-- BEGIN GENERATED: lemonade.jobs_interrupt -->
## `POST /v1/jobs/{id}/interrupt`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Cancels a job's current step now, aborting an in-flight load or chat. The step returns to pending, so resuming re-runs it, and the models the job loaded are unloaded.

Also served at `/api/v0/jobs/{id}/interrupt`, `/api/v1/jobs/{id}/interrupt` and `/v0/jobs/{id}/interrupt`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job, or one that is not queued or running, is answered with `404`.

### Example request

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

### Response format

`200`

```json
{"status": "interrupting"}
```
<!-- END GENERATED: lemonade.jobs_interrupt -->

<!-- BEGIN GENERATED: lemonade.jobs_resume -->
## `POST /v1/jobs/{id}/resume`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Continues a paused job at its next step, or re-runs an interrupted job's pending step after reloading the models the interrupt unloaded.

Also served at `/api/v0/jobs/{id}/resume`, `/api/v1/jobs/{id}/resume` and `/v0/jobs/{id}/resume`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job, or one that is not paused or interrupted, is answered with `404`.

### Example request

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

### Response format

`200`

```json
{"status": "resuming"}
```
<!-- END GENERATED: lemonade.jobs_resume -->

<!-- BEGIN GENERATED: lemonade.jobs_get -->
## `GET /v1/jobs/{id}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Returns a job's full record: its status, the state of each step, and the context the steps have written.

Also served at `/api/v0/jobs/{id}`, `/api/v1/jobs/{id}` and `/v0/jobs/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job is answered with `404`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000007"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs/job-20261007-171503-000007
    ```

### Response format

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

**Field Descriptions:**

- `context` - Each completed step's output under its id, the extracted keys, and inputs.
- `cursor` - Id of the current or next step.
<!-- END GENERATED: lemonade.jobs_get -->

<!-- BEGIN GENERATED: lemonade.jobs_delete -->
## `DELETE /v1/jobs/{id}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Removes a job. An active job is interrupted first, and the models it loaded are unloaded before it disappears.

Also served at `/api/v0/jobs/{id}`, `/api/v1/jobs/{id}` and `/v0/jobs/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from `POST /v1/jobs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job is answered with `404`.

### Example request

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

### Response format

`200`

```json
{"status": "deleted"}
```
<!-- END GENERATED: lemonade.jobs_delete -->
