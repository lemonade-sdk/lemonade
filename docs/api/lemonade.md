# Lemonade API

We have designed a set of Lemonade-specific endpoints to enable client applications by extending the existing cloud-focused APIs (e.g., OpenAI). These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:

- Download models at setup time.
- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Unload models to save memory space.
- Understand system resources and state to make dynamic choices.

<!-- BEGIN GENERATED: lemonade.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/classify`](#post-v1classify) | Score text against a classifier model's labels |
| `POST` | [`/v1/models/check-updates`](#post-v1modelscheck-updates) | Manually check downloaded models for upstream updates |
| `POST` | [`/v1/models/register`](#post-v1modelsregister) | Register or update a user model definition without downloading it |
| `POST` | [`/v1/pull`](#post-v1pull) | Install a model |
| `GET` | [`/v1/downloads`](#get-v1downloads) | List server-owned download jobs |
| `POST` | [`/v1/downloads/control`](#post-v1downloadscontrol) | Pause, cancel, or remove server-owned download jobs |
| `GET` | [`/v1/registry/search`](#get-v1registrysearch) | Search Hugging Face or ModelScope for model repositories |
| `GET` | [`/v1/pull/variants`](#get-v1pullvariants) | List the installable variants of a model repository |
| `POST` | [`/v1/delete`](#post-v1delete) | Delete a model |
| `POST` | [`/v1/load`](#post-v1load) | Load a model |
| `POST` | [`/v1/unload`](#post-v1unload) | Unload a model |
| `POST` | [`/v1/audio/generations`](#post-v1audiogenerations) | Generate audio (music or sound effects) from a text prompt |
| `POST` | [`/v1/3d/generations`](#post-v13dgenerations) | Generate a textured 3D mesh (GLB) from an image |
| `GET` | [`/v1/docs`](#get-v1docs) | List the API reference pages bundled with the running server |
| `GET` | [`/v1/docs/{page}`](#get-v1docspage) | Read one bundled API reference page |
| `GET` | [`/v1/health`](#get-v1health) | Check the server's status and loaded models |
| `GET` | [`/v1/stats`](#get-v1stats) | Read performance statistics from the last request |
| `GET` | [`/v1/system-stats`](#get-v1system-stats) | Read the host's resource usage |
| `GET` | [`/metrics`](#get-metrics) | Serve metrics for Prometheus to scrape |
| `GET` | [`/v1/system-info`](#get-v1system-info) | Read the host's hardware and backend support |
| `POST` | [`/v1/install`](#post-v1install) | Install or update a backend, or register a cloud provider |
| `POST` | [`/v1/install/dry-run`](#post-v1installdry-run) | Resolve backend install metadata without downloading the backend asset |
| `POST` | [`/v1/uninstall`](#post-v1uninstall) | Remove a backend or cloud provider |
| `POST` | [`/v1/cloud/auth`](#post-v1cloudauth) | Set an in-memory API key for a cloud provider |
| `DELETE` | [`/v1/cloud/auth/{provider}`](#delete-v1cloudauthprovider) | Clear the in-memory API key for a cloud provider |
| `WS` | [`/logs/stream`](#log-streaming-api-websocket) | Log Streaming |
| `GET` | [`/live`](#get-live) | Check server liveness for load balancers and orchestrators |
| `POST` | [`/v1/jobs`](#post-v1jobs) | Create a job |
| `GET` | [`/v1/jobs`](#get-v1jobs) | List jobs |
| `POST` | [`/v1/jobs/{id}/pause`](#post-v1jobsidpause) | Pause a job |
| `POST` | [`/v1/jobs/{id}/interrupt`](#post-v1jobsidinterrupt) | Interrupt a job |
| `POST` | [`/v1/jobs/{id}/resume`](#post-v1jobsidresume) | Resume a job |
| `GET` | [`/v1/jobs/{id}`](#get-v1jobsid) | Read a job |
| `DELETE` | [`/v1/jobs/{id}`](#delete-v1jobsid) | Delete a job |
<!-- END GENERATED: lemonade.summary -->

Some Lemonade endpoints live on other pages: the per-model [`/v1/models/{id}/files`](openai.md#get-v1modelsidfiles) and [`/v1/models/{id}/options`](openai.md#get-v1modelsidoptions) extensions sit with the OpenAI models endpoints, routing and [`/v1/routing/validate`](router.md#post-v1routingvalidate) are on the [Router API](router.md) page, and endpoints for clients bundled with the server are on the [Internal API](internal.md) page.

<!-- BEGIN GENERATED: lemonade.classify -->
## `POST /v1/classify`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Runs an encoder text classifier, such as a PII, prompt-safety or domain classifier, on a string and returns a score for each label. It serves `onnxruntime` models, both sequence classifiers (one label set) and token classifiers (aggregated span labels), and provides the classification that the router's `classifier` condition uses.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | No | The classifier model. Optional when exactly one classification model is loaded; that model is used. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to classify. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Return only the highest-scoring `k` labels. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `text` | No | Alias for `input`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Supported Models

Single-sequence encoder families: BERT, DistilBERT, RoBERTa, XLM-RoBERTa, DeBERTa (v1/v2), ELECTRA, ALBERT and CamemBERT. A stock `optimum-cli export onnx` directory of one of these works as is.

A servable model directory holds `model.onnx`, `tokenizer.json` and `config.json`. The `config.json` is always required: its architecture is checked against the families above, so an unsupported family such as XLNet is rejected at load time rather than served with wrong scores. Label names (`id2label`), normalization and the token budget come from the same config, and an optional `manifest.json` overrides them. Without a manifest, inference assumes single-label softmax, so a multi-label (sigmoid) model declares `problem_type: multi_label_classification` in its config or ships a `manifest.json`.

Some upstream models declare only generic `LABEL_<n>` label names; their model card explains them.

A malformed request (invalid JSON, a missing `input`/`text`, non-string fields, a non-positive `top_k`) answers `400` before any model loads.

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

<!-- BEGIN GENERATED: lemonade.models_check_updates -->
## `POST /v1/models/check-updates`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Checks downloaded models for newer commits in their registry. It is the manual counterpart to the startup update check and runs even when `auto_check_model_updates` is `false`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

With `offline` set to `true`, it answers `409` without any network request.

The `lemonade check-updates` command uses this endpoint.

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

- `status` - `failed` when any model's check failed.
- `updates_available` - How many models have an update.
- `models` - Models with a newer upstream commit.
- `failed_models` - Each model whose check failed, with its error.
<!-- END GENERATED: lemonade.models_check_updates -->

<!-- BEGIN GENERATED: lemonade.models_register -->
## `POST /v1/models/register`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Registers or updates a `user.*` model definition in `user_models.json` without downloading any files, for clients that register and install as separate steps. [`POST /v1/pull`](#post-v1pull) registers the same way and then downloads.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | Non-empty namespaced model name under `user.*`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | Yes | Recipe that loads the model, such as `llamacpp`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | Main checkpoint. Optional, since registration records only metadata and some model types have no local weights. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Named checkpoints for multi-checkpoint models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry or local source. Remote values are `huggingface` or `modelscope`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Model labels; see [Model Labels](./openai.md#model-labels). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Collections only: the component model names, each already registered. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `400` answers a body that is not a JSON object, a missing `model_name`, a name outside the `user.` namespace or with a reserved `extra.`/`builtin.` prefix, a missing `recipe`, an embedded `models` array, and malformed `checkpoint`, `checkpoints`, `source` or `components` values.

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

- `model_name` - The ID `/v1/models` lists for the model.
- `canonical_model_name` - The `user.*` name the request registered.
- `model`
  - `id` - Model identifier for loading and inference requests.
  - `created` - Unix time the model entry was created.
  - `object` - Always `"model"`.
  - `owned_by` - Always `"lemonade"`.
  - `checkpoint` - The main checkpoint in the model's registry; see `registry_source`.
  - `recipe` - Recipe used to load the model, e.g. `llamacpp` or `flm`.
  - `size` - Size in GB; omitted when unknown.
  - `max_context_window` - Largest text context the model supports, read from local metadata: downloaded GGUF models and installed FLM text models.
  - `context_length` - Number of tokens the model can handle in one request. Uses the loaded value when the model is running, the configured `ctx_size`, or the discovered cloud provider context length otherwise (omitted when none is known).
  - `max_output_tokens` - Cloud models only: the most tokens one completion can generate, when the provider reports it.
  - `max_completion_tokens` - Cloud models only: `max_output_tokens` under its OpenAI name.
  - `downloaded` - Whether the model is downloaded.
  - `update_available` - Whether the model's registry has a newer commit; `false` except for downloaded registry models.
  - `suggested` - Whether the model is recommended for general use.
  - `labels` - Array of tags describing the model's capabilities and characteristics. See [Model Labels](./openai.md#model-labels) for the full list.
  - `image_defaults` - Image models only: default generation parameters.
    - `steps` - Number of inference steps (e.g., 4 for turbo models, 20 for standard models)
    - `cfg_scale` - Classifier-free guidance scale (e.g., 1.0 for turbo models, 7.5 for standard models)
    - `width` - Default image width in pixels
    - `height` - Default image height in pixels
    - `soft_pixel_cap` - Largest output area, in pixels, that images/edits infers from the first reference image when size is omitted.
  - `components` - Omni collections only (`recipe: "collection.omni"`): the names of its component models, in order.
  - `models` - Omni collections only: each component's full model object, in the order of `components`, so an exported collection can be imported elsewhere through [`/v1/pull`](./lemonade.md#post-v1pull).
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

<!-- BEGIN GENERATED: lemonade.pull -->
## `POST /v1/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Installs a model from the built-in registry, or registers a new `user.*` model from a Hugging Face or ModelScope checkpoint and installs it.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `stream` | No | If `true`, returns Server-Sent Events (SSE) with download progress. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Only applies when `stream=true`. If `false`, the server owns the download and answers at once with a snapshot of its job, which [`GET /v1/downloads`](#get-v1downloads) and [`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` | Yes | A [registered model](https://lemonade-server.ai/models.html) to install, or a new `user.*` name to register, such as `user.Phi-4-Mini-GGUF`. The `user.` namespace keeps registered models from colliding with built-in ones. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | No | Required when registering: the recipe that loads and runs the model, such as `llamacpp`, or `collection.omni` for an omni collection. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | The `main` checkpoint to install. A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Checkpoints by role, for multi-checkpoint models: `main`, plus `mmproj` for vision, `draft` for dflash, eagle and multi-token prediction, and `text_encoder` and `vae` for image generation, each when `main` does not embed it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reasoning` | No | `true` adds the `reasoning` label, for a reasoning model such as DeepSeek. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vision` | No | `true` adds the `vision` label, for a model that accepts images. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `embedding` | No | `true` adds the `embeddings` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reranking` | No | `true` adds the `reranking` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mmproj` | No | Multimodal projector file for a vision model, in the `checkpoint` repository. The same as `checkpoints.mmproj`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Required for omni collections: the component model names, in order; each must be a regular model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `models` | No | Full model definitions, one per `components` entry, with the same fields as a single registration. Components not yet registered are registered from them; registered ones keep their local definition. Without `models`, every component must already be registered. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Registering only: model labels. A model deploys in exactly one [deployment mode](./openai.md#model-labels): naming a mode the recipe cannot serve, or naming two, through `labels` or the `embedding` and `reranking` flags, is rejected with `400`. Omitting the deployment label applies the recipe's default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry to download from: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `registry_source` | No | Same as `source`; when both are given they must name the same registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `do_not_upgrade` | No | `true` skips checking an already-downloaded model's registry for updates. Only `/v1/pull` checks; loading a model on first use never does. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `local_import` | No | Register model files a client already copied into the Hugging Face cache, without downloading. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `400` answers an invalid definition, an unknown model (with `code: "unknown_model"`), or offline mode (with `code: "lemond_offline"`), and nothing is registered.

### Register and Install a Model

Registering writes the model to `user_models.json` in the Lemonade config directory (`~/.config/lemonade` by default), after which `/v1/models` lists it beside the built-in models. The definition needs a `main` checkpoint, given as `checkpoint` or as the `main` key of `checkpoints`.

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.Phi-4-Mini-GGUF",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M",
    "recipe": "llamacpp"
  }'
```

### Register an Omni Collection

An omni collection bundles several models into one entry that is loaded, pulled and deleted as a unit. Register it with `recipe: "collection.omni"` and `components` instead of a checkpoint. Components not yet downloaded are pulled by the same call.

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

Files written by `lemonade export` and the desktop app's Export button are `/v1/pull` request bodies: POST a file's contents as is to register and install its model. See [Share a collection](../guide/configuration/custom-models.md#share-a-collection-between-machines) for the file format and workflows.

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
    - `type` - Download type.
    - `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
    - `file` - The file downloading now.
    - `file_index` - Its position among the download's files, from 1.
    - `total_files` - How many files the download has.
    - `bytes_downloaded` - Bytes of the current file downloaded so far.
    - `bytes_total` - Size of the current file in bytes.
    - `percent` - Share of the current file downloaded, from 0 to 100.
    - `total_download_size` - Total expected bytes across all files when known.
    - `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
    - `completed_files_bytes` - Bytes from files completed before the current file.
    - `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
    - `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
    - `complete` - `true` when the download completed successfully.
    - `error` - Error message, present only for failed jobs.
    - `code` - `unknown_model` when the model is not in the registry.

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
    - `code` - `unknown_model` when the model is not in the registry.
<!-- END GENERATED: lemonade.pull -->

<!-- BEGIN GENERATED: lemonade.downloads -->
## `GET /v1/downloads`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the server-owned download jobs that [`POST /v1/pull`](#post-v1pull) and [`POST /v1/install`](#post-v1install) start with `stream: true` and `subscribe: false`, so a client can restore its download state after a reload or reconnect.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

Active, paused, cancelled and failed jobs stay listed until a client removes them with [`POST /v1/downloads/control`](#post-v1downloadscontrol). Completed jobs stay listed for 30 seconds, long enough for a client to see them finish.

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
- `type` - Download type.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `status` - Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from 1.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `percent` - Share of the current file downloaded, from 0 to 100.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
- `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - `unknown_model` when the model is not in the registry.
<!-- END GENERATED: lemonade.downloads -->

<!-- BEGIN GENERATED: lemonade.downloads_control -->
## `POST /v1/downloads/control`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Pauses, cancels or removes a server-owned download job listed by [`GET /v1/downloads`](#get-v1downloads).

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` | Yes | The job's `id`, for example `model:Qwen3-0.6B-GGUF`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `action` | Yes | What to do with the job; see [Actions](#actions). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Actions

| Action | Description |
|--------|-------------|
| `pause` | Requests the worker to stop and keeps the job visible as `paused`. The worker may briefly report `running=true` while it unwinds. |
| `cancel` | Requests the worker to stop and marks the job as `cancelled`. Clients should wait for `running=false` before deleting partial files. |
| `remove` | Removes a stopped job from the server registry. If the worker is still running, the server keeps the job visible and treats the request as a cancel request until the worker stops. |

A `pause` or `cancel` for a job that already finished leaves it as it is.

A `400` answers a missing `id` or `action`, invalid JSON, or an unknown action. A `pause` or `cancel` for a job that is not listed answers `404`.

`pause`, `cancel`, and `remove` of a running job answer the job's latest snapshot. Removing a stopped job answers `{"status": "ok"}`, with `"missing": true` when the job was already gone.

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
- `type` - Download type.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from 1.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `percent` - Share of the current file downloaded, from 0 to 100.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
- `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - `unknown_model` when the model is not in the registry.
<!-- END GENERATED: lemonade.downloads_control -->

<!-- BEGIN GENERATED: lemonade.registry_search -->
## `GET /v1/registry/search`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Searches Hugging Face or ModelScope for repositories matching a text query. The results are candidates chosen from registry metadata, so the desktop app's Model Manager checks each one with [`/v1/pull/variants`](#get-v1pullvariants) and offers a download only for repositories whose files pass.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` (query) | Yes | Search text, minimum 3 characters after trimming. `q` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry to search: `huggingface` or `modelscope`, also accepted as `hf` and `ms`. Defaults to `huggingface`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `limit` (query) | No | Maximum number of results, from `1` to `50`. Defaults to `12`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` (query) | No | Only `gguf`, which favors GGUF repositories in search and ranking. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `400` answers a `query` shorter than 3 characters, an invalid `source`, `limit` or `format`, or offline mode (`code: lemond_offline`). A `429` means the upstream registry rate-limited the request, and any other upstream failure answers `502`, with the upstream status code when available.

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

- `source` - The registry searched, by its canonical name.
- `query` - The query as sent.
- `format` - Present only when `format=gguf` was requested.
- `total` - Matches the registry reports; can exceed the results returned.
- `results` - Up to `limit` repositories.
  - `has_gguf` - A hint from registry metadata; [`/v1/pull/variants`](#get-v1pullvariants) checks the files.
<!-- END GENERATED: lemonade.registry_search -->

<!-- BEGIN GENERATED: lemonade.pull_variants -->
## `GET /v1/pull/variants`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Inspects a Hugging Face or ModelScope repository and lists the variants it offers for installation, such as quantizations and sharded folder groups. The `lemonade pull <owner/repo>` command and the desktop app's model search use it to fill in the install form. Lemonade reads public registry metadata, and sends the server's `HF_TOKEN` environment variable, when set, to Hugging Face for gated repositories.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `checkpoint` (query) | Yes | Repository id, e.g. `unsloth/Qwen3-8B-GGUF`. A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A `400` answers a missing or malformed `checkpoint` (it must contain `/`), a `source` that disagrees with the checkpoint URL's registry, or offline mode (`code: lemond_offline`). A checkpoint the registry does not have answers `404`, and any other transport or parsing failure answers `500` with an `error` message.

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
- `recipe` - Suggested recipe: `llamacpp` for a GGUF repo, `ryzenai-llm` for an ONNX RyzenAI repo, `collection.omni` for an exported collection.
- `suggested_name` - Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name.
- `suggested_labels` - Inferred labels: `vision` if any `mmproj-*.gguf` files exist, `mtp` or `dflash` if a variant ships a draft model, plus `embeddings`/`reranking` if those substrings appear in the repo id, and the recipe's deployment label (e.g. `chat`) when no other label names one.
- `mmproj_files` - Bare filenames of `mmproj-*.gguf` files in the repo; pass the first as `mmproj` to [`POST /v1/pull`](#post-v1pull) for a vision model.
- `draft_files` - Bare names of draft-model GGUF files.
- `variants` - Quantizations for the repo. Each entry has `name` (e.g. `Q4_K_M`, `UD-Q4_K_XL`), `primary_file`, `files`, `sharded`, and `size_bytes` (from the registry's file listing). Ranked by frequency of use in `server_models.json` (`Q4_K_M`, `UD-Q4_K_XL`, `Q8_0`, `Q4_0` first, everything else sorted lexicographically).
  - `draft_file` - The draft model paired with this variant, when the repository ships one.
- `size` - Collections only: the collection's total size, from its manifest.
- `component_count` - Collections only: how many models the collection bundles.
<!-- END GENERATED: lemonade.pull_variants -->

<!-- BEGIN GENERATED: lemonade.delete -->
## `POST /v1/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Deletes a model's files from local storage, unloading the model first when it is loaded. Deleting a collection (`recipe: "collection.omni"`) removes only its entry from `user_models.json`; delete its components to free their disk space.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to delete. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A file still held open, for example by a download that was just cancelled, is retried up to 3 times, 5 seconds apart.

An unknown model answers `422`; other failures answer `500`.

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

<!-- BEGIN GENERATED: lemonade.load -->
## `POST /v1/load`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Loads a registered model into memory, downloading it first when needed, so it is ready before the first request.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to load. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `pinned` | No | If `true`, pins the model so least-recently-used eviction skips it. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `save_options` | No | If `true`, saves the options this load uses as the model's `recipe_options.json` entry, replacing the stored one; an option sent as `null` keeps its stored value. To save options without loading, or to change one option without resending the rest, use [`POST /v1/models/{id}/options`](openai.md#post-v1modelsidoptions). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | llamacpp, flm, ryzenai-llm: context size, or `-1` to size it automatically. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_backend` | No | llamacpp: the llama.cpp backend: `vulkan`, `rocm`, `cuda`, `metal` or `cpu`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_args` | No | llamacpp: extra arguments for llama-server. Flags Lemonade sets itself are rejected, with their aliases: `-m`, `--port`, `--ctx-size`, `--jinja`, `--device`, `--mmproj`, `--model-draft`, `--embeddings` and `--reranking`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_backend` | No | whispercpp: the whisper.cpp backend, such as `cpu`, `npu`, `vulkan`, `rocm` or `metal`. [`/v1/system-info`](#get-v1system-info) lists the ones this system supports. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_args` | No | whispercpp: extra arguments for whisper-server, such as `--convert`. `-m`, `--model` and `--port` are rejected. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | sd-cpp: inference steps per image. Defaults to `20`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | sd-cpp: classifier-free guidance scale. Defaults to `7.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `width` | No | sd-cpp: image width in pixels. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | sd-cpp: image height in pixels. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `merge_args` | No | If `true` (the default), server-wide `*_args` still apply when the request sends `*_args`. If `false`, the request's `*_args` replace every server-wide argument instead of adding to them. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Loading a collection (`recipe: "collection.omni"`) loads each component in turn with the component's own saved options; options in the request are not forwarded. A `collection.router` model loads nothing until a request routes to a candidate.

Loading a model that is already loaded with the same options does nothing; different options reload it.

### Option Priority

Each recipe option comes from the first of these layers that sets it:

1. The `load` request
2. The model's saved options in `recipe_options.json`
3. Server-wide settings in `config.json`, or the environment variables that override them (see [Server Configuration](../guide/configuration/README.md))
4. Lemonade's built-in defaults

An option sent as `null` skips the saved value for this load without changing `recipe_options.json`. A `*_args` in the request replaces the model's saved args for this load.

Every option the model's recipe accepts can be passed; the [Backend Reference](../dev/backends-reference.md#recipe-options) lists them per recipe.

Load failures answer with the same error object as inference requests: `404` for an unknown or unsupported model, `409` when pinned models fill every slot, and `500` when the backend fails to start.

### Saved Options

`recipe_options.json`, in the Lemonade config directory (`~/.config/lemonade` by default), holds each model's saved options, keyed by the full model name including any `user.` or `extra.` prefix:

```json
{
  "user.Qwen2.5-Coder-1.5B-Instruct": {
    "ctx_size": 16384,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "-np 2 -kvu"
  },
  "whisper-large-v3-turbo-q8_0.bin": {
    "whispercpp_backend": "npu",
    "whispercpp_args": "--convert"
  }
}
```

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/load" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/load \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF"}'
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

<!-- BEGIN GENERATED: lemonade.unload -->
## `POST /v1/unload`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Unloads a model from memory, freeing it while `lemond` keeps running.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` | No | The model to unload; omit it to unload every model. `model` is accepted as an alias. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unparsable body also unloads every model. A model that is not loaded answers `404`.

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

<!-- BEGIN GENERATED: lemonade.audio_generations -->
## `POST /v1/audio/generations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates an audio clip from a text prompt. The model decides the kind of audio: music with ACE-Step models such as `ACE-Step-Music`, sound effects with ThinkSound models such as `ThinkSound-SFX`. This is a Lemonade extension; OpenAI's audio endpoints cover only speech and transcription.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The audio-generation model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text description of the music or sound effect to generate. For music, this is the style description: genre, mood, tempo, instruments, and voice. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `lyrics` | No | Lyrics to sing (ACE-Step only). When present and not empty, the track is generated with vocals singing these lyrics. Omitting the field, an empty string, or the sentinel `[Instrumental]` (any case) produces an instrumental track. See [Lyrics](#lyrics) below for the expected format. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vocal_language` | No | BCP-47 language code of the lyrics, e.g. `en`, `fr`, `ja` (ACE-Step only). Defaults to `en`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `duration` | No | Length of the clip in seconds. Defaults to the backend's own default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Number of inference steps. Lower is faster, higher can improve quality. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg` | No | Classifier-free guidance strength (ThinkSound only). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only `wav`, the format the backend produces, is accepted; other values answer `400`. Defaults to `wav`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Generation runs on the GPU (Vulkan, ROCm or CUDA) and takes from seconds for a short sound effect to minutes for full-length music, depending on duration and hardware.

### Lyrics

ACE-Step vocals are a two-stage pipeline inside the backend: a language model first turns the style description and lyrics into audio codes, then the diffusion synthesizer renders those codes into audio. The instrumental path skips the language-model stage entirely, which also means lyrics embedded in the `prompt` field are treated as style text and never sung. Vocal generations take noticeably longer than instrumental ones of the same duration because of the extra language-model pass.

Format the `lyrics` value the way the ACE-Step authors recommend:

- Mark each song section with a structure tag on its own line: `[verse]`, `[chorus]`, `[bridge]`, `[intro]`, `[outro]`.
- Write one sung phrase per line and separate sections with a blank line.
- Describe the voice ("gentle female vocals", "raspy male baritone") in `prompt`, not in the lyrics.
- Lyrics may be in any supported language; set `vocal_language` to match.

A failed request answers with a JSON `error` object: `400` for an invalid request, `404` for an unknown model, `500` when the backend reports an error, and `502` when it produces no output.

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

=== "PowerShell (music with vocals)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/audio/generations" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "ACE-Step-Music",
          "prompt": "warm acoustic folk ballad, fingerpicked guitar, gentle female vocals",
          "lyrics": "[verse]\nMoonlight spills across the floor\nShadows dancing by the door\n\n[chorus]\nWe sing until the morning light\nCarried on the wind tonight",
          "duration": 30,
          "seed": 42
        }' `
      -OutFile output.wav
    ```

=== "Bash (music with vocals)"

    ```bash
    curl http://localhost:13305/v1/audio/generations \
      -H "Content-Type: application/json" \
      -d '{
          "model": "ACE-Step-Music",
          "prompt": "warm acoustic folk ballad, fingerpicked guitar, gentle female vocals",
          "lyrics": "[verse]\nMoonlight spills across the floor\nShadows dancing by the door\n\n[chorus]\nWe sing until the morning light\nCarried on the wind tonight",
          "duration": 30,
          "seed": 42
        }' \
      --output output.wav
    ```

### Response format

=== "Sound effect responses"

    `200`, `Content-Type: audio/wav`, 884,780 bytes

=== "Music with vocals responses"

    `200`, `Content-Type: audio/wav`, 8,640,044 bytes
<!-- END GENERATED: lemonade.audio_generations -->

<!-- BEGIN GENERATED: lemonade.3d_generations -->
## `POST /v1/3d/generations`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Generates a textured 3D mesh, as a glTF-binary (`.glb`) file, from a PNG, JPEG, BMP or GIF image. It serves TRELLIS models such as `TRELLIS-3D` and is a Lemonade extension.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The 3D-generation model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | Base64-encoded input image (optionally a `data:` URL). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `resolution` | No | Cascade resolution: `512`, `1024` or `1536`. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `bg_removal` | No | Background removal mode: `threshold` or `birefnet`. Use `birefnet` for photos with real backgrounds. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `uv` | No | UV atlas method. `xatlas` (default) unwraps every face into its own atlas space for the best texture quality, but slows down on large meshes. `box` is a faster six-plane projection that can leave small artifacts in concave regions. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only `glb`, the format the backend produces, is accepted; other values answer `400`. Defaults to `glb`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Reconstruction runs on the GPU (Vulkan, ROCm or CUDA) and takes minutes; higher `resolution` values take longer.

A failed request answers with a JSON `error` object: `400` for an invalid request, `404` for an unknown model, `500` when the backend reports an error, and `502` when it produces no output.

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

<!-- BEGIN GENERATED: lemonade.docs -->
## `GET /v1/docs`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the API reference pages bundled with the server, which describe the running version and need no internet access. Fetch this index first, then read the pages it lists.

Requires `LEMONADE_API_KEY` when it is set.

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
  - `url` - Where to read the page. Clients follow it rather than building URLs, so a release can add pages without breaking them.
<!-- END GENERATED: lemonade.docs -->

<!-- BEGIN GENERATED: lemonade.docs_page -->
## `GET /v1/docs/{page}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reads one page from the [`GET /v1/docs`](#get-v1docs) index, as `text/markdown`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `page` (path) | Yes | The `id` from the [`GET /v1/docs`](#get-v1docs) index, which mirrors the page's path on the documentation website. The `.md` suffix is optional. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown page answers `404`.

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

<!-- BEGIN GENERATED: lemonade.health -->
## `GET /v1/health`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports the server's status, including the models it has loaded.

Requires `LEMONADE_API_KEY` when it is set.

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
  "version": "2026.43.0~1689.0292987c",
  "model_loaded": "Qwen3-0.6B-GGUF",
  "update_check_done": true,
  "all_models_loaded": [
    {
      "model_name": "Qwen3-0.6B-GGUF",
      "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "last_use": 66265129,
      "type": "llm",
      "device": "gpu",
      "pinned": false,
      "is_busy": false,
      "is_streaming": false,
      "backend_url": "http://127.0.0.1:8001/v1",
      "pid": 193678,
      "launch_command": ["~/.cache/lemonade/bin/llamacpp/vulkan/llama-server", "-m", "~/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf", "--ctx-size", "40960", "--port", "8001", "--jinja", ... 13 more],
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

- `status` - Always `"ok"`.
- `version` - Lemonade Server version.
- `model_loaded` - The most recently used loaded model, or `null` when none is loaded.
- `update_check_done` - Whether the startup check for model updates on Hugging Face has finished. Until it has, models' `update_available` can be incomplete.
- `all_models_loaded` - Every loaded model.
  - `model_name` - Name of the loaded model
  - `checkpoint` - The model's main checkpoint.
  - `last_use` - Monotonic timestamp of last access (load or inference), in milliseconds
  - `type` - Model type: `llm`, `embedding`, `reranking`, `transcription`, `image`, `tts`, `audio-generation`, `classification` or `mesh`.
  - `device` - Space-separated devices the model runs on: `cpu`, `gpu`, `npu`, or a combination such as `gpu npu`.
  - `pinned` - Whether the model is pinned, which exempts it from auto-eviction
  - `is_busy` - Whether the model has active requests or maintenance in progress
  - `is_streaming` - Whether the model is generating output tokens: true from the first chunk until every streaming request completes
  - `backend_url` - URL of the backend process serving the model.
  - `pid` - Process ID of the backend process serving the model.
  - `launch_command` - The command that started the model's backend, program first, with the values actually used: an automatic `ctx_size` appears as a number, and flags Lemonade added are included. Absent for cloud models.
  - `recipe` - Recipe used to load the model, e.g. `llamacpp` or `flm`.
  - `recipe_options` - Options the model was loaded with, such as `ctx_size` or `llamacpp_args`.
  - `residency_class` - `standard`, or `routing_helper` for a model a router policy keeps resident.
  - `slot_pool` - The loaded-model limit the model counts against, or `unmetered`.
  - `watchdog_reset` - Whether the backend watchdog has reset this backend.
- `pinned_models` - Counts of pinned models currently loaded in memory per model type (e.g., `llm`, `embedding`, etc.)
- `max_models` - Maximum number of models that can be loaded simultaneously per type (set via `max_loaded_models` in [Server Configuration](../guide/configuration/README.md))
  - `llm` - Maximum LLM/chat models
  - `embedding` - Maximum embedding models
  - `reranking` - Maximum reranking models
  - `transcription` - Maximum speech-to-text models
  - `image` - Maximum image models
  - `tts` - Maximum text-to-speech models
  - `classification` - Maximum classification models
- `websocket_port` - Port of the dedicated WebSocket server for the [Realtime Audio Transcription API](./openai.md#ws-realtime) and [Log Streaming API](#log-streaming-api-websocket), present while it runs; the main port serves both APIs too. The port is OS-assigned, or set by the `websocket_port` setting in [Server Configuration](../guide/configuration/README.md).
- `telemetry` - Telemetry state.
  - `enabled` - Whether telemetry collection is active
  - `captures` - The parts of each request telemetry captures, present when `enabled` is `true`.
- `pinned_helper_models` - Pinned routing-helper models of each type.
<!-- END GENERATED: lemonade.health -->

<!-- BEGIN GENERATED: lemonade.stats -->
## `GET /v1/stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports performance statistics for the most recent request, plus counters since server start.

Requires `LEMONADE_API_KEY` when it is set.

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
  "time_to_first_token": 0.010258,
  "tokens_per_second": 330.7515771574438,
  "input_tokens": 15,
  "output_tokens": 327,
  "prompt_tokens": 15,
  "cache_tokens": 0,
  "request_count_total": 1,
  "input_tokens_total": 15,
  "output_tokens_total": 327,
  "prompt_tokens_total": 15,
  "cache_tokens_total": 0,
  "routing_decisions_total": 0,
  "routing_switches_total": 0,
  "live": {"active_requests": 0, "estimated_tokens_per_second": 0.0}
}
```

**Field Descriptions:**

- `time_to_first_token` - Time in seconds until the first token was generated
- `tokens_per_second` - Generation speed in tokens per second
- `input_tokens` - Number of tokens processed
- `output_tokens` - Number of tokens generated
- `prompt_tokens` - Total prompt tokens including cached tokens
- `cache_tokens` - Prompt tokens served from the backend's prefix cache on the last request (llama.cpp `timings.cache_n`, or `usage.prompt_tokens_details.cached_tokens` / Responses-API `input_tokens_details.cached_tokens` from OpenAI-compatible cloud providers). `null` when the last request did not report cache usage
- `request_count_total` - Requests served since server start
- `input_tokens_total` - Input tokens since server start
- `output_tokens_total` - Output tokens since server start
- `prompt_tokens_total` - Prompt tokens since server start
- `cache_tokens_total` - Prompt tokens served from the prefix cache since server start
- `routing_decisions_total` - Routing decisions made by `collection.router` dispatch
- `routing_switches_total` - Routing decisions that changed a conversation's routed model (a proxy for route ping-pong; conversations are identified by a hash of the system prompt and first user message)
- `live` - Streams generating output right now. The rate is estimated from streamed text, not counted, so use the last-request fields above for exact numbers. Status and keepalive events are ignored, and both values drop to zero about 3 seconds after output stops or as soon as a request ends.
  - `active_requests` - Streams that produced output in the last few seconds.
  - `estimated_tokens_per_second` - Their combined generation rate, estimated.
<!-- END GENERATED: lemonade.stats -->

<!-- BEGIN GENERATED: lemonade.system_stats -->
## `GET /v1/system-stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports the host's resource usage as the server process measures it, for clients and dashboards that want lightweight telemetry without scraping Prometheus.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

`primary_gpu_id` and `gpus` describe the individual host GPUs. `gpu_percent` and `vram_gb` come from the primary GPU: the one with the largest known GPU-accessible memory capacity (ties broken by stable ID), whatever its current utilization. It is not necessarily the GPU a model recipe targets. `vram_gb` keeps its historical name but reports GPU memory usage, which can be shared system memory on AMD APUs. Device IDs stay stable across ordinary telemetry failures.

GPU, VRAM and NPU telemetry depends on the operating system and installed drivers. On Linux, AMD and NVIDIA GPUs are enumerated; on Windows, NVIDIA GPUs through NVML; on macOS `gpus` is empty. NVML does not report whether memory is dedicated or shared, so NVIDIA GPUs report `memory_type` as `unknown`.

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
  - `memory_type` - `dedicated` for the GPU's own memory, `shared` for system memory, or `unknown`.
  - `gpu_percent` - GPU utilization percentage, or `null` when unavailable
  - `memory_total_gb` - GPU-accessible memory capacity, in GiB, or `null` when unavailable
  - `memory_used_gb` - GPU memory currently in use, in GiB, or `null` when unavailable
- `npu_percent` - NPU utilization percentage, or `null` when unavailable
<!-- END GENERATED: lemonade.system_stats -->

<!-- BEGIN GENERATED: lemonade.metrics -->
## `GET /metrics`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Serves Lemonade's metrics in the Prometheus text exposition format, for Prometheus to scrape.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Polling and Refresh Rate

`/metrics` has no refresh timer of its own: each scrape renders the server's state at that moment. Prometheus's `scrape_interval` sets how often that happens, for example:

```yaml
global:
  scrape_interval: 10s
```

Grafana reads from Prometheus, not from this endpoint, so a dashboard's refresh rate sets how often it queries Prometheus, not how often Prometheus scrapes Lemonade.

### Metric Families

The names, types, labels and help text are defined by the `metrics.describe(...)` calls in [`src/cpp/server/prometheus_metrics.cpp`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/server/prometheus_metrics.cpp). Unsupported, unavailable, null, NaN and infinite values are omitted rather than emitted as samples.

### llama.cpp Backend Metrics

Lemonade starts llama.cpp backends with metrics enabled and, for each loaded `llamacpp` model, makes a best-effort scrape of the backend's private `/metrics` endpoint. Those metrics are renamed under the `lemonade_llamacpp_*` prefix and labeled with the same model metadata as `lemonade_model_info`. A failed backend scrape does not fail the response.

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

<!-- BEGIN GENERATED: lemonade.system_info -->
## `GET /v1/system-info`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports the host's hardware, the devices Lemonade can use, and which backends each recipe supports on this system.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Backend Tiers and Formats

| Tier | Meaning |
|------|---------|
| `core` | Supported by Lemonade's maintainers for production use. |
| `experimental` | Provided by Lemonade's maintainers, not supported for production use. |
| `guest` | Developed and published outside Lemonade's maintainers. Use at your own risk. The models of a recipe whose only backends on this system are guest backends stay hidden from [`GET /v1/models`](./openai.md#get-v1models) with `show_all=true` until one of those backends is installed. |

| Format | Meaning |
|--------|---------|
| `native` | A compiled executable. |
| `python` | Python, packaged with its own interpreter. |
| `container` | An OCI image, run by a pre-installed Podman or Docker. |

The same backend can carry different labels on different operating systems. Each entry in a recipe's `backends` object reports the labels for this system's operating system.

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
  - One entry per recipe, such as `llamacpp` or `flm`.
    - `default_backend` - Preferred backend selected by server policy for this system (present when at least one backend is not `unsupported`)
    - `experimental` - `true` when the `default_backend`'s tier is `experimental` or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
    - `backends` - Available backends for this recipe
      - One entry per backend.
        - `devices` - List of devices **on this system** that support this backend (empty if not supported)
        - `state` - Backend lifecycle state: `unsupported`, `installable`, `update_required`, or `installed`
        - `message` - Human-readable status text for GUI and CLI users. Required for `unsupported`, `installable`, and `update_required`; empty for `installed`.
        - `action` - Actionable user instruction string. For install/update cases this is typically an exact CLI command; for other states it may be empty or another actionable value (for example, a URL).
        - `version` - Installed or configured backend version (when available)
        - `tier` - `core`, `experimental`, or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
        - `format` - `native`, `python`, or `container`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
- `cloud` - Cloud providers installed on this server.
  - `providers` - One entry per installed provider.
    - `name` - Provider name used as the model-name prefix (e.g. `fireworks`).
    - `base_url` - Persisted base URL from `config.json`.
    - `auth_header_name` - Header this provider's API key is sent in (default `Authorization`).
    - `auth_header_prefix` - Value prefix placed before the key (default `Bearer `).
    - `wire_format` - Request/response shape this provider speaks: `openai` (default) or `anthropic`.
    - `env_var` - Canonical environment variable name for this provider's API key (e.g. `LEMONADE_FIREWORKS_API_KEY`). The variable's *name* is reported, never its value.
    - `env_var_set` - `true` if the env var is set in `lemond`'s environment.
    - `runtime_key_set` - `true` if an in-memory key has been supplied through [`POST /v1/cloud/auth`](#post-v1cloudauth) since lemond started.
    - `models_discovered` - Number of chat-capable models currently in the catalog for this provider.
- `unavailable_recipes` - Recipes all of whose models are filtered out on this host. Reported separately so recipes stays the same on every host; dynamic-model backends (cloud, flm) are never listed.
- `no_fetch_executables` - Whether the server is configured not to download backend executables.
<!-- END GENERATED: lemonade.system_info -->

<!-- BEGIN GENERATED: lemonade.install -->
## `POST /v1/install`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Installs a recipe's backend, updating it when the installed version is outdated, or registers a cloud provider when `backend` is `"cloud"`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: the recipe, such as `llamacpp`, `flm`, `whispercpp`, `sd-cpp` or `ryzenai-llm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Local backends: the backend within the recipe, such as `vulkan`, `rocm`, `cpu` or `default`. Cloud providers: `"cloud"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Local backends: if `true`, sends progress as server-sent events. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `force` | No | Local backends: if `true`, installs a backend this system does not support anyway. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, required: a short identifier matching `[a-z0-9_-]+`, such as `fireworks`, used as the provider's model-name prefix. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `base_url` | No | Cloud providers, required: the provider's base URL, such as one ending in `/v1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | No | Cloud providers: an API key, held in `lemond`'s memory. The provider's environment variable takes precedence; see [`POST /v1/cloud/auth`](#post-v1cloudauth). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Cloud providers: must be `true` to send an API key to an `http://` base URL. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_name` | No | Cloud providers: the header that carries the API key. Defaults to `"Authorization"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_prefix` | No | Cloud providers: the text before the key in that header. Defaults to `"Bearer "`; pass `""` for gateways that expect the bare key. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `wire_format` | No | Cloud providers: `"openai"` or `"anthropic"`. An `"anthropic"` provider is served only through [`POST /v1/messages`](./anthropic.md#post-v1messages). Defaults to `"openai"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Local backends with `stream: true`: `false` makes the download server-owned and answers at once with a snapshot of its job, which [`GET /v1/downloads`](#get-v1downloads) and [`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A backend that needs manual setup on this system answers with the setup guide's URL instead of installing.

A local install without both `recipe` and `backend`, or of a backend this system does not support and without `force`, answers `400` with an `error` string; an invalid cloud provider field answers `400` with an `error` object. Any other failure, including invalid JSON, answers `500` with an `error` string.

### Install a Cloud Provider
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Registers a cloud chat provider. The base URL is persisted to `config.json`; the optional `api_key` lives in `lemond` process memory only and is cleared on restart. See the [Cloud Offload guide](../guide/configuration/cloud.md) for the full workflow.

Optional fields are applied only when present in the request body. Re-installing a provider without them keeps its stored values, so updating just the `base_url` never resets a custom auth header or the `allow_insecure_http` opt-in.

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

    - `models_discovered` - Chat models discovered with the resolved API key; `0` when no key resolves.
    - `auth_state`
      - `env_var_set` - Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`.
      - `runtime_key_set` - Whether a key is held in `lemond`'s memory.
    - `warnings` - Present when the base URL is `http://`, or when the provider's environment variable overrode `api_key`.
    - `warning` - The warnings joined into one string, for older clients.
    - `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
    - `type` - Download type.
    - `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
    - `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
    - `file` - The file downloading now.
    - `file_index` - Its position among the download's files, from 1.
    - `total_files` - How many files the download has.
    - `bytes_downloaded` - Bytes of the current file downloaded so far.
    - `bytes_total` - Size of the current file in bytes.
    - `percent` - Share of the current file downloaded, from 0 to 100.
    - `total_download_size` - Total expected bytes across all files when known.
    - `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
    - `completed_files_bytes` - Bytes from files completed before the current file.
    - `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
    - `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
    - `complete` - `true` when the download completed successfully.
    - `error` - Error message, present only for failed jobs.
    - `code` - `unknown_model` when the model is not in the registry.

=== "Streaming responses"

    `200`

    ```text
    event: complete
    data: {"file": "llama-b11393-bin-ubuntu-vulkan-x64.tar.gz", "file_index": 1, "total_files": 1, "bytes_downloaded": 0, "bytes_total": 0, "total_download_size": 0, "bytes_previously_downloaded": 0, "percent": 100}
    ```

    **Field Descriptions:**

    - `status` - Only on a complete event for an operation that had nothing to download.
    - `error` - Only on an error event.
    - `code` - `unknown_model` when the model is not in the registry.
<!-- END GENERATED: lemonade.install -->

<!-- BEGIN GENERATED: lemonade.install_dry_run -->
## `POST /v1/install/dry-run`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Resolves the release asset that [`POST /v1/install`](#post-v1install) would download for a recipe/backend pair, without downloading or installing anything. Resolution reads local configuration and, for a backend version set to `latest`, GitHub release metadata.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | Yes | The recipe, such as `llamacpp`, `whispercpp` or `vllm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | The backend within the recipe, such as `vulkan`, `rocm` or `rocm-nightly`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `arch` | No | A ROCm GPU architecture, such as `gfx1201`, to resolve for in place of this host's detected one, so CI can check an architecture's asset on a runner without that GPU. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A device ISA may resolve to a family target name used by the release repository, for example `gfx1201` to `gfx120X`. That mapping is defined by `rocm_asset_families` in `backend_versions.json`.

Lemonade does not check that the asset exists; fetch `url`, or its split-archive manifest, to verify it.

A `400` answers a missing or empty `recipe` or `backend`. Invalid JSON, or install-parameter resolution failing (an unknown recipe/backend pair, an unsupported platform, unavailable architecture detection, failed version resolution), answers `500`. Errors carry an `error` string, plus `arch` when it was parsed before the failure.

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

- `recipe` - The requested recipe.
- `backend` - The requested backend.
- `arch` - The requested `arch`, or `""` when it was omitted.
- `repo` - GitHub repository of the release.
- `version` - Release version: the pin in `backend_versions.json` unless runtime version policy overrides it.
- `filename` - Release asset name.
- `url` - Download URL built from `repo`, `version` and `filename`.
- `supported` - Whether Lemonade's recipe/backend support matrix accepts the architecture. An unsupported `arch` still resolves, with `false`; without `arch`, a successful answer is always `true`.
- `supports_split_archive` - Whether the recipe supports assets published as multiple archive parts. When `true`, the real download path can consult a `.partcount` manifest.
<!-- END GENERATED: lemonade.install_dry_run -->

<!-- BEGIN GENERATED: lemonade.uninstall -->
## `POST /v1/uninstall`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Removes a recipe's backend, unloading the models that use it first, or a cloud provider when `backend` is `"cloud"`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: the recipe. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | Local backends: the backend within the recipe. Cloud providers: `"cloud"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, required: the installed provider's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A local uninstall without both `recipe` and `backend`, or a cloud uninstall without `provider`, answers `400`. Any other failure, including invalid JSON, answers `500` with an `error` string.

### Uninstall a Cloud Provider
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Removes the provider record from `config.json`, drops its in-memory API key, if any, and evicts every discovered model for that provider from the cache. A provider that was never installed answers `404`.

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

<!-- BEGIN GENERATED: lemonade.cloud_auth -->
## `POST /v1/cloud/auth`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Sets an API key for an installed cloud provider and refreshes the provider's discovered models. The key is held in `lemond`'s memory only, never written to disk, and is gone after a restart; for a key that persists, set `LEMONADE_<PROVIDER>_API_KEY` in `lemond`'s environment instead.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `provider` | Yes | The installed provider's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | Yes | The API key. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Opt in to sending the key to a provider whose base URL is `http://`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

When `LEMONADE_<PROVIDER>_API_KEY` is set, it takes precedence: the request answers `409` without storing the key, with `error.type` `auth_conflict` and the `env_var` that took precedence.

A `400` answers invalid JSON, a missing or empty `provider` or `api_key`, a non-boolean `allow_insecure_http`, or a provider whose base URL is `http://` when neither this request nor the provider's install set `allow_insecure_http` (code `insecure_http_requires_opt_in`). A provider that is not installed answers `404`; install it with [`POST /v1/install`](#post-v1install) and `backend: "cloud"` first.

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

- `auth_state`
  - `env_var_set` - Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`.
  - `runtime_key_set` - Whether a key is held in `lemond`'s memory.
- `models_discovered` - Chat models discovered with the key; `0` when discovery fails.
- `warnings` - Present when the provider's base URL is `http://`.
- `warning` - The warnings joined into one string, for older clients.
<!-- END GENERATED: lemonade.cloud_auth -->

<!-- BEGIN GENERATED: lemonade.cloud_auth_provider -->
## `DELETE /v1/cloud/auth/{provider}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Clears the API key [`POST /v1/cloud/auth`](#post-v1cloudauth) set for a provider. A `LEMONADE_<PROVIDER>_API_KEY` in the environment stays in effect; without one, the provider's discovered models leave the catalog, since nothing can authenticate them.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `provider` (path) | Yes | The installed provider's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

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

**Field Descriptions:**

- `cleared_runtime_key` - `false` when no in-memory key was present, for example when the only key came from the environment variable.
- `auth_state`
  - `env_var_set` - Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`.
  - `runtime_key_set` - Whether a key is held in `lemond`'s memory.
<!-- END GENERATED: lemonade.cloud_auth_provider -->

## Log Streaming API (WebSocket)
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Stream server logs over WebSocket. Clients connect, send a subscribe message, and receive a snapshot of recent log history followed by live log entries as they occur.

### Connection

Connect on the main HTTP port:

```
ws://localhost:13305/logs/stream
```

The dedicated WebSocket port that the [Realtime Audio Transcription API](./openai.md#ws-realtime) uses accepts the same connection; [`/v1/health`](#get-v1health) reports it as `websocket_port`. When `LEMONADE_API_KEY` is set, pass `?api_key=KEY` as a query parameter.

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

<!-- BEGIN GENERATED: lemonade.live -->
## `GET /live`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Confirms the server process is up, for load balancers and orchestrators. Unlike [`GET /v1/health`](#get-v1health), it inspects no models or backends, so it is safe to poll at high frequency.

Requires no API key.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

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

<!-- BEGIN GENERATED: lemonade.jobs_create -->
## `POST /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Creates a job: a sequence of server operations that runs in the background, passing data between steps and branching on results. A job survives client disconnects and server restarts, and can be paused, interrupted, resumed, read and deleted.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `name` | No | Display name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | The steps, in order; see [Recipe: steps](../dev/job-system.md#recipe-steps). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `definition` | No | Alternative to `steps`: an object whose `steps` holds the steps. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `inputs` | No | Values the steps read as `context.inputs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An invalid step graph answers `400` at creation. While the job store holds 50 jobs that are all still active or resumable, a new job answers `429` until one is deleted or finishes.

Exclusive ops (`load`, `unload`, `chat`) hold a Router slot, so normal traffic queues behind a running job.

[`docs/dev/job-system.md`](../dev/job-system.md) specifies the step schema, op set and lifecycle, and [`docs/dev/job-expression-language.md`](../dev/job-expression-language.md) the `when`/`branch` expression grammar.

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

Requires `LEMONADE_API_KEY` when it is set.

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

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from [`POST /v1/jobs`](#post-v1jobs). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job, or one that is not queued or running, answers `404`.

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

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from [`POST /v1/jobs`](#post-v1jobs). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job, or one that is not queued or running, answers `404`.

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

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from [`POST /v1/jobs`](#post-v1jobs). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job, or one that is not paused or interrupted, answers `404`.

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

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from [`POST /v1/jobs`](#post-v1jobs). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job answers `404`.

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

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from [`POST /v1/jobs`](#post-v1jobs). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

An unknown job answers `404`.

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
