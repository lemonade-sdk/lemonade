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
| `POST` | [`/v1/3d/generations`](#post-v13dgenerations) | Generate a textured 3D mesh from an image |
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
| `input` or `text` | Yes | Text to classify. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `top_k` | No | Return only the highest-scoring `k` labels. Defaults to every label. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A malformed request, such as one with a blank `input` or a non-positive `top_k`, is rejected before any model loads.

### Supported Models

Single-sequence encoder families: BERT, DistilBERT, RoBERTa, XLM-RoBERTa, DeBERTa (v1/v2), ELECTRA, ALBERT and CamemBERT. A stock `optimum-cli export onnx` directory of one of these works as is.

A servable model directory holds `model.onnx`, `tokenizer.json` and `config.json`. The `config.json` is always required: its architecture is checked against the families above, so an unsupported family such as XLNet is rejected at load time rather than served with wrong scores. Label names (`id2label`), normalization and the token budget come from the same config, and an optional `manifest.json` overrides them. Without a manifest, inference assumes single-label softmax, so a multi-label (sigmoid) model declares `problem_type: multi_label_classification` in its config or ships a `manifest.json`.

Some upstream models declare only generic `LABEL_<n>` label names; their model card explains them.

### Example

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

**Response:** `200 OK`

```json
{
  "object": "classification",
  "model": "Phishing-Email-Detection-ONNX",
  "labels": {"LABEL_0": 5.558096745517105e-05, ... 3 more}
}
```

### Response format

- `object` - Always `classification`.
- `model` - The model that classified the input, including one chosen because it was the only one loaded.
- `labels` - Each label's score, keyed by label name.
  - The label's score, in `[0, 1]`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model` is omitted and not exactly one classification model is loaded |
| `400` |  | The backend rejects the request, for example because `model` is not a classification model, which is found only after it loads |
| `404` |  | The model is unloaded while the request waits for it |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The backend's response lacks a field the route returns, such as `choices` or `labels` |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: lemonade.classify -->

<!-- BEGIN GENERATED: lemonade.models_check_updates -->
## `POST /v1/models/check-updates`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Checks downloaded models for newer commits in their registry. It is the manual counterpart to the startup update check and runs even when `auto_check_model_updates` is `false`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

The `lemonade check-updates` command uses this endpoint.

### Example

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

**Response:** `200 OK`

```json
{"status": "success", "updates_available": 0, "models": [], "failed_models": {}}
```

### Response format

- `status` - `failed` when any model's check failed.
- `updates_available` - How many models have an update.
- `models` - Models with a newer upstream commit.
- `failed_models` - Each model whose check failed, with its error.

### Errors

| Status | When |
|--------|------|
| `409` | `offline` is set to `true`; no network request is made |
| `500` | The request fails unexpectedly |
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
| `checkpoint` | No | Main checkpoint. Model types with no local weights have none. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Named checkpoints for multi-checkpoint models. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry or local source. Remote values are `huggingface` or `modelscope`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Model labels; see [Model Labels](./openai.md#model-labels). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Collections only: the component model names, each already registered. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

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

### Response format

- `status` - Always `success`.
- `model_name` - The ID [`GET /v1/models`](./openai.md#get-v1models) lists for the model.
- `canonical_model_name` - The `user.*` name the request registered.
- `model` - The registered model, in the shape [`GET /v1/models/{id}`](./openai.md#get-v1modelsid) returns.
  - `id` - Model identifier for loading and inference requests.
  - `created` - Always `1234567890`, for OpenAI compatibility.
  - `object` - Always `"model"`.
  - `owned_by` - Always `"lemonade"`.
  - `checkpoint` - The main checkpoint in the model's registry; see `registry_source`.
  - `recipe` - Recipe used to load the model, e.g. `llamacpp` or `flm`.
  - `size` - Size in GB; omitted when unknown.
  - `max_context_window` - Largest text context the model supports, read from local metadata: downloaded GGUF models and installed FLM text models.
  - `context_length` - Tokens the model handles in one request: the loaded `ctx_size` while it runs, else the configured `ctx_size`, else `max_context_window`; omitted when none is known.
  - `max_output_tokens` - Cloud models only: the most tokens one completion can generate, when the provider reports it.
  - `max_completion_tokens` - Cloud models only: `max_output_tokens` under its OpenAI name.
  - `downloaded` - Whether the model is downloaded.
  - `update_available` - Whether the model's registry has a newer commit; `false` except for downloaded registry models.
  - `suggested` - Whether the model is recommended for general use.
  - `labels` - The model's capabilities and characteristics; see [Model Labels](./openai.md#model-labels).
  - `image_defaults` - Image models only: default generation parameters.
    - `steps` - Number of inference steps, such as `4` for turbo models or `20` for standard models.
    - `cfg_scale` - Classifier-free guidance scale, such as `1.0` for turbo models or `7.5` for standard models.
    - `width` - Default image width in pixels.
    - `height` - Default image height in pixels.
    - `sampling_method` - Default sampler, when the model sets one.
    - `flow_shift` - Default flow shift, when the model sets one.
    - `soft_pixel_cap` - Largest output area, in pixels, that `images/edits` infers from the first reference image when `size` is omitted.
  - `components` - The names of a collection's component models, in order; empty for other models.
  - `models` - Omni collections only: each component's full model object, in the order of `components`, so an exported collection can be imported elsewhere through [`/v1/pull`](./lemonade.md#post-v1pull).
  - `checkpoints` - Every checkpoint by role, such as `main`, `mmproj`, `draft`, `text_encoder` or `vae`.
    - The checkpoint that fills the role named by its key.
  - `source` - Where the model came from: a registry name, `local_upload`, `local_path` or `extra_models_dir`.
  - `registry_source` - Remote registry the checkpoint downloads from: `huggingface` or `modelscope`.
  - `recipe_options` - The model's own options: its image defaults and registry options, with what was saved in `recipe_options.json` on top.
  - `cloud_provider` - Cloud models only: the provider that serves the model.
  - `cost_input_per_million` - Cloud models only: USD per million input tokens, when the provider reports it.
  - `cost_output_per_million` - Cloud models only: USD per million output tokens, when the provider reports it.
  - `system_prompt` - Omni collections only: the collection's system prompt override.
  - `audio_defaults` - Audio models only: default generation parameters.
  - `version` - Router collections only: the policy document's version.
  - `routing` - Router collections only: the routing policy.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The definition is malformed and nothing is registered, for example a name outside the `user.` namespace or with a reserved `extra.`/`builtin.` prefix, an empty `recipe` where one is required, `checkpoints` without `main`, an unsupported `source`, `components` that are empty or name the collection itself, another collection or an unregistered model, or an invalid routing policy |
| `400` |  | A field not listed above has the wrong JSON type |
| `500` |  | The definition is invalid in another way and nothing is registered, for example a GGUF `checkpoint` without a variant, a recipe this system cannot run, or labels that name two deployment modes |
<!-- END GENERATED: lemonade.models_register -->

<!-- BEGIN GENERATED: lemonade.pull -->
## `POST /v1/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Installs a model from the built-in registry, or registers a new `user.*` model from a Hugging Face or ModelScope checkpoint and installs it.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `stream` | No | `true` sends the response as server-sent events as it progresses; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Only applies when `stream` is `true`. If `false`, the server owns the download and answers at once with a snapshot of its job, which [`GET /v1/downloads`](#get-v1downloads) and [`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control. With `true`, closing the stream cancels the download. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model_name` or `model` | Yes | A [registered model](https://lemonade-server.ai/models.html) to install, or a new `user.*` name to register, such as `user.Phi-4-Mini-GGUF`. The `user.` namespace keeps registered models from colliding with built-in ones. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `recipe` | No | Required when registering: the recipe that loads and runs the model, such as `llamacpp`, or `collection.omni` for an omni collection. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoint` | No | The `main` checkpoint to install. A Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `checkpoints` | No | Checkpoints by role, for multi-checkpoint models: `main`, plus `mmproj` for vision, `draft` for dflash, eagle and multi-token prediction, and `text_encoder` and `vae` for image generation, each when `main` does not embed it. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reasoning` | No | `true` adds the `reasoning` label, for a reasoning model such as DeepSeek. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vision` | No | `true` adds the `vision` label, for a model that accepts images. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `embedding` | No | `true` adds the `embeddings` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `reranking` | No | `true` adds the `reranking` label. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `mmproj` | No | Multimodal projector file for a vision model, as a filename in the `checkpoint` repository, such as `mmproj-model-f16.gguf`. `checkpoints.mmproj` takes the same file as `repository:filename`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `components` | No | Required for an omni collection registered without a `checkpoint`: the component model names, in order; each must be a regular model, not a collection. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `models` | No | Full model definitions, one per `components` entry, with the same fields as a single registration. Components not yet registered are registered from them; registered ones keep their local definition. Without `models`, every component must already be registered. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `labels` | No | Registering only: model labels; see [Model Labels](./openai.md#model-labels). Omitting the deployment label applies the recipe's default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` | No | Registry to download from: `huggingface` or `modelscope`. Defaults to the server's `default_model_source`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `registry_source` | No | Same as `source`; when both are given they must name the same registry. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `do_not_upgrade` | No | `true` skips checking an already-downloaded model's registry for updates. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `local_import` | No | Register model files a client already copied into the Hugging Face cache, without downloading. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Pulling an already-downloaded model downloads any newer commit from its registry. Loading a model never checks for updates, and [`POST /v1/models/check-updates`](#post-v1modelscheck-updates) only reports them.

With `stream`, the error event or the job reports an unknown model.

In offline mode a new definition is still registered, but nothing is downloaded.

### Register and Install a Model

Registering writes the model to `user_models.json` in the Lemonade config directory (`~/.config/lemonade` by default), after which [`GET /v1/models`](./openai.md#get-v1models) lists it beside the built-in models. The definition needs a `main` checkpoint, given as `checkpoint` or as the `main` key of `checkpoints`.

### Register an Omni Collection

An omni collection bundles several models into one entry that is loaded and pulled as a unit. Register it with `recipe: "collection.omni"` and `components` instead of a checkpoint. The same call pulls each component.

### Import an Exported Model File

Files written by `lemonade export` and the desktop app's Export button are `/v1/pull` request bodies: POST a file's contents as is to register and install its model. See [Share a collection](../guide/configuration/custom-models.md#share-a-collection-between-machines) for the file format and workflows.

### Examples

#### Job

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

**Response:** `200 OK`

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

#### Non-streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF"}'
    ```

**Response:** `200 OK`

```json
{"status": "success", "model_name": "Qwen3-0.6B-GGUF"}
```

#### Streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "Qwen3-0.6B-GGUF", "stream": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{"model_name": "Qwen3-0.6B-GGUF", "stream": true}'
    ```

**Response:** `200 OK`

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

#### Register

Registers `user.Qwen3-0.6B-Q4` from a Hugging Face checkpoint and downloads it.

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model_name": "user.Qwen3-0.6B-Q4",
          "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
          "recipe": "llamacpp"
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{
          "model_name": "user.Qwen3-0.6B-Q4",
          "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
          "recipe": "llamacpp"
        }'
    ```

**Response:** `200 OK`

```json
{"status": "success", "model_name": "user.Qwen3-0.6B-Q4"}
```

#### Collection

Registers `user.MyKit`, an omni collection of three built-in models, and pulls each of them.

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model_name": "user.MyKit",
          "recipe": "collection.omni",
          "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/pull \
      -H "Content-Type: application/json" \
      -d '{
          "model_name": "user.MyKit",
          "recipe": "collection.omni",
          "components": ["Qwen3-0.6B-GGUF", "Whisper-Tiny", "SD-Turbo"]
        }'
    ```

**Response:** `200 OK`

```json
{"status": "success", "model_name": "user.MyKit"}
```

### Response format

#### JSON

##### Installed

Without `stream`, or with `local_import`: the model is installed or registered.

- `status` - Always `success`.
- `model_name` - The installed model.
- `message` - Only with `local_import`: a confirmation that the files were registered.

##### Download job

With `stream: true` and `subscribe: false`: the server-owned download job.

- `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
- `type` - Download type.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `status` - Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from `1`.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `percent` - Share of the current file downloaded, from `0` to `100`.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
- `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - `unknown_model` when the model is not in the registry.

#### Event stream

A `progress` event reports one file; a `complete` event follows the last one, and an `error` event replaces it on failure.

- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from `1`.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `total_download_size` - Total expected bytes across all files, or `0` when unknown.
- `bytes_previously_downloaded` - Bytes of the current file already on disk before this download, when resuming or skipping existing data.
- `percent` - Share of the current file downloaded, from `0` to `100`.
- `status` - Only on a complete event for an operation that had nothing to download.
- `error` - Only on an error event.
- `code` - `unknown_model` when the model is not in the registry.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `source` or `registry_source` names an unsupported source, or the checkpoint URLs disagree with it or with each other |
| `400` |  | The definition is malformed and nothing is registered, for example a name outside the `user.` namespace or with a reserved `extra.`/`builtin.` prefix, an empty `recipe` where one is required, `checkpoints` without `main`, an unsupported `source`, `components` that are empty or name the collection itself, another collection or an unregistered model, or an invalid routing policy |
| `400` | `lemond_offline` | `offline` is set to `true` |
| `400` | `unknown_model` | Without `stream`, the model is not registered and its name has no `user.` prefix |
| `400` |  | Labels or `embedding`/`reranking` flags name a deployment mode the recipe cannot serve, or name two |
| `500` |  | Another failure occurs, for example a failed download, a model this system cannot run, a new `user.*` model without `recipe` or `checkpoint`, or a GGUF `checkpoint` without a variant; a new definition stays registered |
| `503` | `registry_unavailable` | Without `stream`, the model registry rate-limits or is temporarily unavailable; the `Retry-After` header and the body's `upstream_status_code` carry the details |
<!-- END GENERATED: lemonade.pull -->

<!-- BEGIN GENERATED: lemonade.downloads -->
## `GET /v1/downloads`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the server-owned download jobs that [`POST /v1/pull`](#post-v1pull) and [`POST /v1/install`](#post-v1install) start with `stream: true` and `subscribe: false`, so a client can restore its download state after a reload or reconnect.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

Active, paused, cancelled and failed jobs stay listed until a client removes them with [`POST /v1/downloads/control`](#post-v1downloadscontrol). Completed jobs stay listed for 30 seconds, long enough for a client to see them finish.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/downloads"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/downloads
    ```

**Response:** `200 OK`

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

### Response format

- `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
- `type` - Download type.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `status` - Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from `1`.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `percent` - Share of the current file downloaded, from `0` to `100`.
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

`pause` and `cancel` answer the job's latest snapshot, as does `remove` for a running job. Removing a stopped job answers `{"status": "ok"}`, with `"missing": true` when the job was already gone.

### Examples

#### Pause

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

**Response:** `200 OK`

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

#### Remove

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/downloads/control" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"id": "model:Qwen3-1.7B-GGUF", "action": "remove"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/downloads/control \
      -H "Content-Type: application/json" \
      -d '{"id": "model:Qwen3-1.7B-GGUF", "action": "remove"}'
    ```

**Response:** `200 OK`

```json
{"status": "ok", "missing": true}
```

### Response format

#### Removed

`remove` of a stopped job, or of an `id` with no job: the job is gone.

- `status` - Always `ok`.
- `missing` - Only when no job had that `id`.

#### Snapshot

`pause`, `cancel`, or `remove` of a running job: the job's latest snapshot.

- `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
- `type` - Download type.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `status` - Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from `1`.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `percent` - Share of the current file downloaded, from `0` to `100`.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
- `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - `unknown_model` when the model is not in the registry.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `id` names no download job and `action` is not `remove` |
<!-- END GENERATED: lemonade.downloads_control -->

<!-- BEGIN GENERATED: lemonade.registry_search -->
## `GET /v1/registry/search`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Searches Hugging Face or ModelScope for repositories matching a text query. The results are candidates chosen from registry metadata, so the desktop app's Model Manager checks each one with [`GET /v1/pull/variants`](#get-v1pullvariants) and offers a download only for repositories whose files pass.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` or `q` (query) | Yes | Search text, minimum 3 characters after trimming. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `source` (query) | No | Registry to search: `huggingface` or `modelscope`, also accepted as `hf` and `ms`. Defaults to `huggingface`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `limit` (query) | No | Maximum number of results, from `1` to `50`. Defaults to `12`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` (query) | No | Only `gguf`, which limits a Hugging Face search to GGUF repositories and favors them in a ModelScope search. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/registry/search?source=modelscope&query=qwen&format=gguf"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/registry/search?source=modelscope&query=qwen&format=gguf"
    ```

**Response:** `200 OK`

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
    ... 11 more
  ]
}
```

### Response format

- `source` - The registry searched, by its canonical name.
- `query` - The query, trimmed.
- `format` - Present only when `format=gguf` was requested.
- `total` - Matches the registry reports; can exceed the results returned.
- `results` - Up to `limit` repositories.
  - `repository_id` - Repository id, such as `unsloth/Qwen3-8B-GGUF`.
  - `display_name` - Name to show for the repository.
  - `source` - The registry the repository is on.
  - `repository_type` - The registry's repository type.
  - `description` - The repository's description.
  - `tags` - The repository's tags.
  - `task` - The repository's task, such as `text-generation`.
  - `downloads` - Download count the registry reports.
  - `likes` - Like count the registry reports.
  - `has_gguf` - A hint from registry metadata.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` |  | `query` (or `q`) is shorter than 3 characters after trimming |
| `400` |  | `source` names an unsupported registry, `limit` is not an integer from 1 to 50, or `format` is not `gguf` |
| `400` | `lemond_offline` | `offline` is set to `true` |
| `429` |  | The upstream registry rate-limits the request; `error.upstream_status_code` is its status |
| `502` |  | The upstream registry answers with another error; `error.upstream_status_code` is its status |
| `502` |  | The search fails with no upstream status, for example on a network error |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-8B-GGUF"
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-8B-GGUF"
    ```

**Response:** `200 OK`

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
    ... 24 more
  ]
}
```

### Response format

- `checkpoint` - Echoed input (normalized to `owner/repo` when given as a URL).
- `source` - Registry the variants came from.
- `recipe` - Suggested recipe: `llamacpp` for a GGUF repo, `ryzenai-llm` for an ONNX RyzenAI repo, `collection.omni` for an exported collection.
- `repo_kind` - Kind of repository: `gguf`, `onnx-ryzenai` or `collection`.
- `suggested_name` - Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name.
- `suggested_labels` - Inferred labels: `vision` if any GGUF file name contains `mmproj`, `mtp` or `dflash` if a variant ships a draft model, plus `embeddings` or `reranking` if the repo id contains `embed` or `rerank`, and the recipe's deployment label (e.g. `chat`) when no other label names one.
- `mmproj_files` - Bare filenames of the repo's GGUF files whose name contains `mmproj`; pass the first as `mmproj` to [`POST /v1/pull`](#post-v1pull) for a vision model.
- `draft_files` - Bare names of draft-model GGUF files.
- `variants` - Installable variants, ranked by how often `server_models.json` uses each quantization: `Q4_K_M`, `UD-Q4_K_XL`, `Q8_0` and `Q4_0` first, then the rest in lexicographic order.
  - `name` - Variant name, such as `Q4_K_M`.
  - `primary_file` - The file to load.
  - `draft_file` - The draft model paired with this variant, when the repository ships one.
  - `files` - Every file in the variant.
  - `sharded` - Whether the variant is split across files.
  - `size_bytes` - Total size, from the registry's file listing.
- `size` - Collections only: the collection's total size, from its manifest.
- `component_count` - Collections only: how many models the collection bundles.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` |  | `checkpoint` is missing or empty |
| `400` |  | `checkpoint` is a URL whose registry disagrees with `source` |
| `400` |  | `checkpoint` does not contain `/` |
| `400` |  | `source` names an unsupported registry, or the Hugging Face GGUF repository is one llama.cpp cannot run |
| `400` | `lemond_offline` | `offline` is set to `true` |
| `404` |  | The registry has no such checkpoint |
| `500` |  | Another failure occurs, for example a repository with no GGUF, ONNX RyzenAI or collection files, or a network error |
<!-- END GENERATED: lemonade.pull_variants -->

<!-- BEGIN GENERATED: lemonade.delete -->
## `POST /v1/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Deletes a model's files from local storage, unloading the model first when it is loaded. A `user.*` model's entry in `user_models.json` is removed too. Deleting a collection (`recipe: "collection.omni"`) removes only that entry; delete its components to free their disk space.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` or `model` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A file still held open, for example by a download that was just cancelled, is retried up to 3 times, 5 seconds apart.

### Example

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

**Response:** `200 OK`

```json
{"status": "success", "message": "Deleted model: user.Phi-4-Mini-GGUF"}
```

### Response format

- `status` - Always `success`.
- `message` - A confirmation naming the deleted model.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `422` |  | The model is not in the registry, or this system does not support it |
| `500` |  | Deleting fails for another reason, for example for an `extra.` model, a file that stays locked after the retries, or a failed unload |
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
| `pinned` | No | If `true`, pins the model so least-recently-used eviction skips it; `false` unpins it. When omitted, a loaded model keeps its pin state and a new load uses the model's saved `pinned` option, else `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `save_options` | No | If `true`, saves the options sent in this request as the model's `recipe_options.json` entry, replacing the stored one, so an option the request omits is no longer saved; one sent as `null` keeps its stored value. To save options without loading, or to change one option without resending the rest, use [`POST /v1/models/{id}/options`](openai.md#post-v1modelsidoptions). Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Context size: a positive whole number, or `-1` to size it automatically even when the server-wide `ctx_size` is a specific number. Only recipes that take a context size use it, such as `llamacpp`, `flm`, `ryzenai-llm` and `vllm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_backend` | No | `llamacpp`: the llama.cpp backend: `auto`, `vulkan`, `rocm`, `cuda`, `metal`, `cpu`, or `system` for a `llama-server` already installed where supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `llamacpp_args` | No | `llamacpp`: extra arguments for `llama-server`. Flags Lemonade sets itself are rejected, with their aliases: `-m`, `--port`, `--ctx-size`, `--jinja`, `--device`, `--mmproj`, `--model-draft`, `--embeddings`, `--reranking` and `--metrics`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_backend` | No | `whispercpp`: the whisper.cpp backend, such as `cpu`, `npu`, `vulkan`, `rocm` or `metal`. [`/v1/system-info`](#get-v1system-info) lists the ones this system supports. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `whispercpp_args` | No | `whispercpp`: extra arguments for `whisper-server`, such as `--convert`. `-m`, `--model` and `--port` are rejected. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | `sd-cpp`: inference steps per image. Defaults to the model's own, else `20`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | `sd-cpp`: classifier-free guidance scale. Defaults to the model's own, else `7.0`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `width` | No | `sd-cpp`: image width in pixels. Defaults to the model's own, else `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | `sd-cpp`: image height in pixels. Defaults to the model's own, else `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `merge_args` | No | If `true`, server-wide `*_args` still apply when the request sends `*_args`. If `false`, the request's `*_args` are the only arguments passed: server-wide, saved and architecture `*_args` are dropped, as are the arguments Lemonade adds to `llama-server` by default, such as `--parallel 1`. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Loading a collection (`recipe: "collection.omni"`) loads each component in turn with the component's own saved options; options in the request are not forwarded. A `collection.router` model loads nothing until a request routes to a candidate.

Loading a model that is already loaded with the same options does nothing; different options reload it.

### Option Priority

Each recipe option comes from the first of these layers that sets it:

1. The `load` request
2. The model's own options: its registry and image defaults, with its saved `recipe_options.json` entry on top
3. Lemonade's defaults for the model's GGUF architecture
4. Server-wide settings in `config.json`, or the environment variables that override them (see [Server Configuration](../guide/configuration/README.md))
5. Lemonade's built-in defaults

An option sent as `null` skips the saved value for this load without changing `recipe_options.json`. A `*_args` option instead combines the flags of every layer, each flag from the highest layer that sets it, except that a `*_args` in the request replaces the model's and the architecture's `*_args` for this load.

Every option the model's recipe accepts can be passed; the [Backend Reference](../dev/backends-reference.md#recipe-options) lists them per recipe.

### Saved Options

`recipe_options.json`, in the Lemonade config directory (`~/.config/lemonade` by default), holds each model's saved options, keyed by the model name with its source prefix: `user.` or `extra.` as in the name, and `builtin.` for a built-in model:

```json
{
  "user.Qwen2.5-Coder-1.5B-Instruct": {
    "ctx_size": 16384,
    "llamacpp_backend": "vulkan",
    "llamacpp_args": "-np 2 -kvu"
  },
  "builtin.Whisper-Large-v3-Turbo": {
    "whispercpp_backend": "npu",
    "whispercpp_args": "--convert"
  }
}
```

### Examples

#### Model

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

**Response:** `200 OK`

```json
{
  "status": "success",
  "model_name": "Qwen3-0.6B-GGUF",
  "checkpoint": "unsloth/Qwen3-0.6B-GGUF:Q4_0",
  "recipe": "llamacpp"
}
```

#### Collection

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/load" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model_name": "LMX-Omni-5.5B-Lite"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/load \
      -H "Content-Type: application/json" \
      -d '{"model_name": "LMX-Omni-5.5B-Lite"}'
    ```

**Response:** `200 OK`

```json
{"status": "success", "model_name": "LMX-Omni-5.5B-Lite", "recipe": "collection.omni"}
```

### Response format

#### Model

A model with a backend of its own.

- `status` - Always `success`.
- `model_name` - The loaded model.
- `checkpoint` - The model's main checkpoint.
- `recipe` - The recipe that loaded the model, such as `llamacpp`.

#### Collection

A collection, which has no checkpoint of its own.

- `status` - Always `success`.
- `model_name` - The loaded collection.
- `recipe` - The collection's recipe: `collection.omni` or `collection.router`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: lemonade.load -->

<!-- BEGIN GENERATED: lemonade.unload -->
## `POST /v1/unload`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Unloads a model from memory, freeing it while `lemond` keeps running.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model_name` or `model` | No | The model to unload; omit it to unload every model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{"status": "success", "message": "Model unloaded successfully", "model_name": "Qwen3-0.6B-GGUF"}
```

### Response format

- `status` - Always `success`.
- `message` - A confirmation that one model, or every model, was unloaded.
- `model_name` - Only when one model was unloaded.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | The named model is not loaded, which includes an omni collection, whose components are unloaded by their own names |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: lemonade.unload -->

<!-- BEGIN GENERATED: lemonade.audio_generations -->
## `POST /v1/audio/generations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Generates an audio clip from a text prompt. The model decides the kind of audio: music with ACE-Step models such as `ACE-Step-Music`, sound effects with ThinkSound models such as `ThinkSound-SFX`. This is a Lemonade extension; OpenAI's audio endpoints cover only speech and transcription.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text description of the music or sound effect to generate. For music, this is the style description: genre, mood, tempo, instruments, and voice. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `lyrics` | No | Lyrics to sing (ACE-Step only). When present and not empty, the track is generated with vocals singing these lyrics. Omitting the field, an empty string, or the sentinel `[Instrumental]` (any case) produces an instrumental track. See [Lyrics](#lyrics) below for the expected format. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `vocal_language` | No | BCP-47 language code of the lyrics, e.g. `en`, `fr`, `ja` (ACE-Step only). Defaults to `en`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `duration` | No | Length of the clip in seconds. Defaults to the backend's own default. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Number of inference steps; lower is faster, higher can improve quality. Defaults to the model's own setting, else the backend's. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg` | No | Classifier-free guidance strength (ThinkSound only). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. Defaults to the backend's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only `wav`, the format the backend produces, is accepted. Defaults to `wav`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Generation runs on the GPU (Vulkan, ROCm or CUDA) and takes from seconds for a short sound effect to minutes for full-length music, depending on duration and hardware.

Errors carry a JSON `error` object.

### Lyrics

ACE-Step vocals are a two-stage pipeline inside the backend: a language model first turns the style description and lyrics into audio codes, then the diffusion synthesizer renders those codes into audio. The instrumental path skips the language-model stage entirely, which also means lyrics embedded in the `prompt` field are treated as style text and never sung. Vocal generations take noticeably longer than instrumental ones of the same duration because of the extra language-model pass.

Format the `lyrics` value the way the ACE-Step authors recommend:

- Mark each song section with a structure tag on its own line: `[verse]`, `[chorus]`, `[bridge]`, `[intro]`, `[outro]`.
- Write one sung phrase per line and separate sections with a blank line.
- Describe the voice ("gentle female vocals", "raspy male baritone") in `prompt`, not in the lyrics.
- Lyrics may be in any supported language; set `vocal_language` to match.

### Examples

#### Sound effect

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

**Response:** `200 OK`, `audio/wav`, 884,780 bytes

#### Music with vocals

=== "PowerShell"

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

=== "Bash"

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

**Response:** `200 OK`, `audio/wav`, 8,640,044 bytes

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` | `model_not_applicable` | `model` is not an audio-generation model |
| `400` |  | `response_format` is not a format `model`'s backend produces, or, when streaming, one it streams |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
| `502` |  | The backend produces no output, for example after it crashes or runs out of GPU memory |
<!-- END GENERATED: lemonade.audio_generations -->

<!-- BEGIN GENERATED: lemonade.3d_generations -->
## `POST /v1/3d/generations`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Generates a textured 3D mesh, as a glTF-binary (`.glb`) file, from an image. It serves TRELLIS models such as `TRELLIS-3D` and is a Lemonade extension.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `image` | Yes | Base64-encoded PNG, JPEG, BMP or GIF image, optionally as a `data:` URL. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `resolution` | No | Cascade resolution: `512`, `1024` or `1536`. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `bg_removal` | No | Background removal mode: `threshold` or `birefnet`. Use `birefnet` for photos with real backgrounds. Defaults to the backend's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `uv` | No | UV atlas method. `xatlas` unwraps every face into its own atlas space for the best texture quality, but slows down on large meshes. `box` is a faster six-plane projection that can leave small artifacts in concave regions. Defaults to `xatlas`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducibility. Defaults to the backend's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `response_format` | No | Output encoding. Only `glb`, the format the backend produces, is accepted. Defaults to `glb`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Reconstruction runs on the GPU (Vulkan, ROCm or CUDA) and takes minutes; higher `resolution` values take longer.

Errors carry a JSON `error` object.

### Example

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

**Response:** `200 OK`, `model/gltf-binary`, 4,534,160 bytes

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `image` is not base64 data, or is a `data:` URL without a base64 payload |
| `400` |  | `image` is not a PNG, JPEG, BMP or GIF |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
| `502` |  | The backend produces no output, for example after it crashes or runs out of GPU memory |
<!-- END GENERATED: lemonade.3d_generations -->

<!-- BEGIN GENERATED: lemonade.docs -->
## `GET /v1/docs`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the API reference pages bundled with the server, which describe the running version and need no internet access. Fetch this index first, then read the pages it lists.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/docs"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/docs
    ```

**Response:** `200 OK`

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
    ... 8 more
  ]
}
```

### Response format

- `version` - Lemonade Server version the pages describe.
- `format` - The media type of every listed page.
- `docs` - The bundled pages.
  - `id` - Page id, mirroring the page's path on the documentation website.
  - `title` - The page's title.
  - `url` - Where to read the page. Clients follow it rather than building URLs, so a release can add pages without breaking them.
  - `bytes` - The page's size in bytes.
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/docs/api/lemonade"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/docs/api/lemonade
    ```

**Response:** `200 OK`

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

### Errors

| Status | When |
|--------|------|
| `404` | `page` names no bundled page |
<!-- END GENERATED: lemonade.docs_page -->

<!-- BEGIN GENERATED: lemonade.health -->
## `GET /v1/health`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports the server's status, including the models it has loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/health"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/health
    ```

**Response:** `200 OK`

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
  "pinned_models": {"classification": 0, ... 6 more},
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
  "pinned_helper_models": {"classification": 0, ... 6 more}
}
```

### Response format

- `status` - Always `"ok"`.
- `version` - Lemonade Server version.
- `model_loaded` - The most recently used loaded model, or `null` when none is loaded.
- `update_check_done` - Whether the startup check for model updates has finished. Until it has, models' `update_available` can be incomplete.
- `all_models_loaded` - Every loaded model.
  - `model_name` - Name of the loaded model
  - `checkpoint` - The model's main checkpoint.
  - `last_use` - Monotonic timestamp of last access (load or inference), in milliseconds
  - `type` - Model type: `llm`, `embedding`, `reranking`, `transcription`, `image`, `tts`, `audio-generation`, `classification` or `mesh`.
  - `device` - Devices the model runs on, joined by `|`: `cpu`, `gpu` and `npu`, such as `gpu|npu`, or `none`.
  - `pinned` - Whether the model is pinned, which exempts it from auto-eviction
  - `is_busy` - Whether the model has active requests or maintenance in progress
  - `is_streaming` - Whether the model is generating output tokens: `true` from the first chunk until every streaming request completes.
  - `backend_url` - URL of the backend process serving the model.
  - `pid` - Process ID of the backend process serving the model.
  - `launch_command` - The command that started the model's backend, program first, with the values actually used: an automatic `ctx_size` appears as a number, and flags Lemonade added are included. Absent for cloud models.
  - `recipe` - Recipe used to load the model, e.g. `llamacpp` or `flm`.
  - `recipe_options` - Options the model was loaded with, such as `ctx_size` or `llamacpp_args`.
  - `residency_class` - `standard`, or `routing_helper` for a model a router policy keeps resident.
  - `slot_pool` - The loaded-model limit the model counts against, or `unmetered`.
  - `status` - Load state: `loading`, `ready`, `in_use`, `downsizing`, `downsized`, `evicting` or `unloaded`.
  - `backend_alive` - Always `true` for a listed model.
  - `backend_health` - `ready`, or `busy` while the backend has a request in flight.
  - `loaded` - Always `true` for a listed model.
  - `watchdog_reset` - Always `false` for a listed model, since a model whose backend the watchdog reset is left out.
  - `watchdog_reset_reason` - Why the watchdog reset the backend; present after a reset.
  - `max_context_window` - Largest context the model supports, when known.
  - `cost_input_per_million` - Cloud models only: USD per million input tokens, when the provider reports it.
  - `cost_output_per_million` - Cloud models only: USD per million output tokens, when the provider reports it.
- `pinned_models` - Loaded pinned models, counted per model type, such as `llm` or `embedding`.
  - How many pinned models of this type are loaded.
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
- `pinned_helper_models` - Loaded pinned routing-helper models, counted per model type.
  - How many pinned routing-helper models of this type are loaded.
<!-- END GENERATED: lemonade.health -->

<!-- BEGIN GENERATED: lemonade.stats -->
## `GET /v1/stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports performance statistics for the most recent request, plus counters since server start.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/stats"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/stats
    ```

**Response:** `200 OK`

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

### Response format

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

### Errors

| Status | When |
|--------|------|
| `500` | The request fails unexpectedly |
<!-- END GENERATED: lemonade.stats -->

<!-- BEGIN GENERATED: lemonade.system_stats -->
## `GET /v1/system-stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports the host's resource usage as the server process measures it, for clients and dashboards that want lightweight telemetry without scraping Prometheus.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

`HEAD` returns `200 OK` with an empty body.

`gpu_percent` and `vram_gb` come from the primary GPU: the one with the largest known GPU-accessible memory capacity (ties broken by ID), whatever its current utilization, and not necessarily the GPU a model recipe targets. `vram_gb` keeps its historical name but reports GPU memory usage, which can be shared system memory on AMD APUs.

GPU, VRAM and NPU telemetry depends on the operating system and installed drivers. On Linux, AMD and NVIDIA GPUs are enumerated; on Windows, NVIDIA GPUs through NVML and AMD GPUs through DXGI, DXCore and PDH; on macOS `gpus` is empty. NVML does not report whether memory is dedicated or shared, so NVIDIA GPUs report `memory_type` as `unknown`.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/system-stats"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/system-stats
    ```

**Response:** `200 OK`

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

### Response format

- `cpu_percent` - System CPU utilization percentage, or `null` when unavailable
- `memory_gb` - System RAM currently in use, in GiB
- `gpu_percent` - GPU utilization percentage, or `null` when unavailable
- `vram_gb` - GPU memory currently in use, in GiB, or `null` when unavailable
- `primary_gpu_id` - `id` of the GPU that `gpu_percent` and `vram_gb` describe, or `null` when no GPU is enumerated
- `gpus` - Every enumerated host GPU, primary first
  - `id` - Device ID, stable across ordinary telemetry failures, such as `amd:0000:c1:00.0`.
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/metrics"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/metrics
    ```

**Response:** `200 OK`

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

### Errors

| Status | When |
|--------|------|
| `500` | The request fails unexpectedly |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/system-info"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/system-info
    ```

**Response:** `200 OK`

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
      "order": 13,
      "display_name": "ACE-Step",
      "web_display_name": "ACE-Step",
      "modality": "Audio generation",
      "selectable_backend": true,
      "uses_ctx_size": false,
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
        ... 2 more
      ],
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
        ... 2 more
      }
    },
    ... 15 more
  },
  "cloud": {"providers": []},
  "unavailable_recipes": [],
  "no_fetch_executables": false
}
```

### Response format

- `OS Version` - Operating system name and version
- `Processor` - CPU model name
- `Physical Memory` - Total RAM
- `model_storage` - Drive-level storage information for the active configured model storage path. Values are reported in bytes for storage meters; this is not a recursive sum of Lemonade model files.
  - `path` - Active model storage path: the `models_dir` setting, or the Hugging Face cache directory when it is `auto`.
  - `used_bytes` - Used bytes on the model-storage drive
  - `total_bytes` - Total capacity of the model-storage drive
  - `free_bytes` - Free bytes available to the Lemonade Server process on the model-storage drive
  - `error` - Only when the drive could not be measured.
- `devices` - Hardware devices detected on the system (no software/support information)
  - `cpu` - CPU information (name, cores, threads)
  - `amd_gpu` - Usable AMD GPUs, both integrated and discrete; empty when there are none.
  - `nvidia_gpu` - NVIDIA GPUs. When none is found, it holds one entry whose `available` is `false` and whose `error` says why.
  - `amd_npu` - The AMD NPU. Always present; `available` is `false` when none is found, and `error` says why.
- `recipes` - Software recipes and their backend support status
  - One entry per recipe, such as `llamacpp` or `flm`.
    - `default_backend` - Preferred backend for this system: the one set in `config.json`, else the first in preference order that this system's devices can run, favoring an installed one. Absent when none is set and none can run here.
    - `experimental` - `true` when the `default_backend`'s tier is `experimental` or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
    - `order` - The recipe's position in Lemonade's backend registry, from `0`.
    - `display_name` - The recipe's name for people, such as `Llama.cpp GPU`.
    - `web_display_name` - The recipe's name on the Lemonade website.
    - `modality` - The kind of model the recipe mainly serves, such as `Text generation`.
    - `selectable_backend` - Whether a `<recipe>_backend` option chooses the recipe's backend.
    - `uses_ctx_size` - Whether the recipe accepts the shared `ctx_size` option.
    - `slot_policy` - How the recipe's models count against loaded-model limits: `standard`, `exclusive_npu` (loading one evicts every NPU model), `coexist_by_type` (one per model type) or `unmetered` (never counted or evicted).
    - `support` - Every backend's operating systems, devices and tier, the same on every host.
    - `options` - The recipe's load options, with each one's CLI flag, default and help text.
    - `backends` - Available backends for this recipe
      - One entry per backend.
        - `devices` - Devices **on this system** that support this backend; empty when none do.
        - `state` - Backend lifecycle state: `unsupported`, `installable`, `update_required`, `update_available` or `installed`.
        - `message` - Human-readable status text for GUI and CLI users. Required for `unsupported`, `installable`, and `update_required`; empty for `installed`.
        - `action` - An instruction for the user: for install and update, usually the exact CLI command; for other states it may be empty or another action, such as a URL.
        - `version` - Installed or configured backend version (when available)
        - `tier` - `core`, `experimental`, or `guest`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
        - `format` - `native`, `python`, or `container`. See [Backend Tiers and Formats](#backend-tiers-and-formats).
        - `release_url` - Where the backend's release asset downloads from.
        - `download_filename` - The release asset's filename.
        - `can_uninstall` - Whether the backend can be uninstalled. Lemonade sets it only on a backend that does not run on this operating system, always to `true`, so a missing value also means `true`.
- `cloud` - Cloud providers installed on this server.
  - `providers` - One entry per installed provider.
    - `name` - Provider name used as the model-name prefix (e.g. `fireworks`).
    - `base_url` - Persisted base URL from `config.json`.
    - `auth_header_name` - Header this provider's API key is sent in (default `Authorization`).
    - `auth_header_prefix` - Value prefix placed before the key (default `Bearer `).
    - `wire_format` - Request/response shape this provider speaks: `openai` (default) or `anthropic`.
    - `env_var` - Canonical environment variable name for this provider's API key (e.g. `LEMONADE_FIREWORKS_API_KEY`). The variable's *name* is reported, never its value.
    - `env_var_set` - `true` if the env var is set in `lemond`'s environment.
    - `runtime_key_set` - `true` if an in-memory key has been supplied through [`POST /v1/cloud/auth`](#post-v1cloudauth) since `lemond` started.
    - `models_discovered` - Number of chat-capable models currently in the catalog for this provider.
    - `allow_insecure_http` - Whether the provider may be reached over `http://`.
    - `warnings` - Problems found with the provider's configuration.
    - `warning` - `warnings` joined into one string, for older clients.
- `unavailable_recipes` - Recipes all of whose models are filtered out on this host. Reported separately so `recipes` stays the same on every host; dynamic-model backends (`cloud`, `flm`) are never listed.
- `no_fetch_executables` - Whether the server is configured not to download backend executables.
<!-- END GENERATED: lemonade.system_info -->

<!-- BEGIN GENERATED: lemonade.install -->
## `POST /v1/install`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Installs or updates a recipe's backend, or registers a cloud provider.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: the recipe, such as `llamacpp`, `flm`, `whispercpp`, `sd-cpp` or `ryzenai-llm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | The backend within the recipe, such as `vulkan`, `rocm`, `cpu` or `default`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` sends the response as server-sent events as it progresses; `false` sends one response once complete. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `force` | No | Local backends: if `true`, installs a backend this system does not support anyway. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | Cloud providers, which take `backend: "cloud"`, required: a short identifier matching `[a-z0-9_-]+`, such as `fireworks`, used as the provider's model-name prefix. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `base_url` | No | Cloud providers, required: the provider's base URL, such as one ending in `/v1`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `api_key` | No | Cloud providers: an API key, held in `lemond`'s memory. The provider's environment variable takes precedence; see [`POST /v1/cloud/auth`](#post-v1cloudauth). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `allow_insecure_http` | No | Must be `true` to send an API key to a provider whose base URL is `http://`; the opt-in is saved with the provider. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_name` | No | Cloud providers: the header that carries the API key. Defaults to `"Authorization"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `auth_header_prefix` | No | Cloud providers: the text before the key in that header. Defaults to `"Bearer "`; pass `""` for gateways that expect the bare key. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `wire_format` | No | Cloud providers: `"openai"` or `"anthropic"`. An `"anthropic"` provider is served only through [`POST /v1/messages`](./anthropic.md#post-v1messages). Defaults to `"openai"`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `subscribe` | No | Only applies when `stream` is `true`. If `false`, the server owns the download and answers at once with a snapshot of its job, which [`GET /v1/downloads`](#get-v1downloads) and [`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control. With `true`, closing the stream cancels the download. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Errors carry an `error` string, except a cloud provider's field that has the right type but fails the provider checks, which carries an `error` object: a missing or empty `provider` or `base_url`, an invalid `provider`, `base_url`, `auth_header_name` or `auth_header_prefix`, or an `http://` base URL without `allow_insecure_http`.

### Install a Cloud Provider
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

The base URL is saved to `config.json`. See the [Cloud Offload guide](../guide/configuration/cloud.md) for the full workflow.

Optional fields are applied only when present in the request body. Re-installing a provider without them keeps its stored values, so updating just the `base_url` never resets a custom auth header or the `allow_insecure_http` opt-in.

### Examples

#### Local

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"recipe": "llamacpp", "backend": "cpu"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/install \
      -H "Content-Type: application/json" \
      -d '{"recipe": "llamacpp", "backend": "cpu"}'
    ```

**Response:** `200 OK`

```json
{"status": "success", "recipe": "llamacpp", "backend": "cpu"}
```

#### Streaming

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

**Response:** `200 OK`

```text
event: complete
data: {"file": "llama-b11393-bin-ubuntu-vulkan-x64.tar.gz", "file_index": 1, "total_files": 1, "bytes_downloaded": 0, "bytes_total": 0, "total_download_size": 0, "bytes_previously_downloaded": 0, "percent": 100}
```

#### Job

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/install" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"recipe": "llamacpp", "backend": "vulkan", "stream": true, "subscribe": false}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/install \
      -H "Content-Type: application/json" \
      -d '{"recipe": "llamacpp", "backend": "vulkan", "stream": true, "subscribe": false}'
    ```

**Response:** `200 OK`

```json
{
  "status": "downloading",
  "id": "backend:llamacpp:vulkan",
  "type": "backend",
  "model_name": "llamacpp:vulkan",
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

#### Cloud

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

**Response:** `200 OK`

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

### Response format

#### JSON

##### Local backend

A local backend installed or already up to date.

- `status` - Always `success`.
- `recipe` - The recipe from the request.
- `backend` - The backend from the request.

##### Manual setup

A backend that needs manual setup on this system, such as `rocm` on a Strix Halo (`gfx1151`) whose Linux kernel lacks the CWSR fix: nothing was installed.

- `action` - The setup guide's URL.
- `recipe` - The recipe from the request.
- `backend` - The backend from the request.

##### Cloud provider

A cloud provider registered.

- `status` - Always `success`.
- `backend` - Always `cloud`.
- `provider` - The provider's name, which prefixes its model names.
- `base_url` - The provider's saved base URL.
- `allow_insecure_http` - Whether an API key may be sent to an `http://` base URL.
- `auth_header_name` - The header that carries the API key.
- `auth_header_prefix` - The text before the key in that header.
- `wire_format` - The API the provider speaks: `openai` or `anthropic`.
- `models_discovered` - Chat models discovered with the resolved API key; `0` when no key resolves.
- `auth_state` - Which API keys are set for the provider.
  - `env_var_set` - Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`.
  - `runtime_key_set` - Whether a key is held in `lemond`'s memory.
- `warnings` - Present when the base URL is `http://`, or when the provider's environment variable overrode `api_key`.
- `warning` - The warnings joined into one string, for older clients.

##### Download job

`stream: true` with `subscribe: false`: the server-owned download job just started.

- `id` - Stable download id. Model downloads use `model:<model_name>`. Backend downloads use `backend:<recipe>:<backend>`.
- `type` - Download type.
- `model_name` - Lemonade model name associated with the job, or `recipe:backend` for a backend job.
- `status` - Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`.
- `running` - Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources.
- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from `1`.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `percent` - Share of the current file downloaded, from `0` to `100`.
- `total_download_size` - Total expected bytes across all files when known.
- `bytes_previously_downloaded` - Bytes already present on disk for the current file when resuming or skipping existing data.
- `completed_files_bytes` - Bytes from files completed before the current file.
- `cumulative_bytes_downloaded` - Bytes downloaded across the whole job.
- `overall_bytes_downloaded` - Older name for `cumulative_bytes_downloaded`.
- `complete` - `true` when the download completed successfully.
- `error` - Error message, present only for failed jobs.
- `code` - `unknown_model` when the model is not in the registry.

#### Event stream

A `progress` event reports one file; a `complete` event follows the last one, and an `error` event replaces it on failure.

- `file` - The file downloading now.
- `file_index` - Its position among the download's files, from `1`.
- `total_files` - How many files the download has.
- `bytes_downloaded` - Bytes of the current file downloaded so far.
- `bytes_total` - Size of the current file in bytes.
- `total_download_size` - Total expected bytes across all files, or `0` when unknown.
- `bytes_previously_downloaded` - Bytes of the current file already on disk before this download, when resuming or skipping existing data.
- `percent` - Share of the current file downloaded, from `0` to `100`.
- `status` - Only on a complete event for an operation that had nothing to download.
- `error` - Only on an error event.
- `code` - `unknown_model` when the model is not in the registry.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | A local request has no `recipe` |
| `400` |  | The backend is not supported on this system and `force` is not set |
| `400` |  | A cloud install is missing `provider` or `base_url`, or one is empty |
| `400` |  | `provider` is longer than 64 characters or not lowercase `[a-z0-9_-]` |
| `400` |  | `base_url` does not start with `https://` or `http://`, has no host, or contains whitespace |
| `400` |  | `auth_header_name` is empty, over 128 characters or not a valid HTTP header name, or `auth_header_prefix` is over 128 characters or contains a control character |
| `400` | `insecure_http_requires_opt_in` | The provider's base URL is `http://`, an API key would be sent to it, and neither this request nor an earlier install sets `allow_insecure_http` |
| `500` |  | The request fails unexpectedly |
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
| `backend` | Yes | The backend within the recipe, such as `vulkan`, `rocm`, `cpu` or `default`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `arch` | No | A ROCm GPU architecture, such as `gfx1201`, to resolve for in place of this host's detected one, so CI can check an architecture's asset on a runner without that GPU. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Errors carry an `error` string.

A device ISA may resolve to a family target name used by the release repository, for example `gfx1201` to `gfx120X`. That mapping is defined by `rocm_asset_families` in `backend_versions.json`.

Lemonade does not check that the asset exists; fetch `url`, or its split-archive manifest, to verify it.

### Example

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

**Response:** `200 OK`

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

### Response format

- `recipe` - The requested recipe.
- `backend` - The requested backend.
- `arch` - The requested `arch`, or `""` when it was omitted.
- `repo` - GitHub repository of the release.
- `version` - Release version: the pin in `backend_versions.json` unless runtime version policy overrides it.
- `filename` - Release asset name.
- `url` - Download URL built from `repo`, `version` and `filename`.
- `supported` - Whether Lemonade's recipe/backend support matrix accepts the architecture. An unsupported `arch` still resolves, with `false`; without `arch`, a successful answer is always `true`.
- `supports_split_archive` - Whether the recipe supports assets published as multiple archive parts. When `true`, the real download path can consult a `.partcount` manifest.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `500` |  | Resolving the install parameters fails, for example for an unknown recipe/backend pair, an unsupported platform or a failed version lookup; the body also carries `arch` when the request set one |
<!-- END GENERATED: lemonade.install_dry_run -->

<!-- BEGIN GENERATED: lemonade.uninstall -->
## `POST /v1/uninstall`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Removes a recipe's backend, unloading the models that use it first, or a cloud provider when `backend` is `"cloud"`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `recipe` | No | Local backends, required: the recipe, such as `llamacpp`, `flm`, `whispercpp`, `sd-cpp` or `ryzenai-llm`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `backend` | Yes | The backend within the recipe, such as `vulkan`, `rocm`, `cpu` or `default`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `provider` | No | The installed cloud provider to remove. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Uninstall a Cloud Provider
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Removes the provider record from `config.json`, drops its in-memory API key, if any, and evicts every discovered model for that provider from the catalog.

### Examples

#### Local

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/uninstall" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"recipe": "llamacpp", "backend": "cpu"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/uninstall \
      -H "Content-Type: application/json" \
      -d '{"recipe": "llamacpp", "backend": "cpu"}'
    ```

**Response:** `200 OK`

```json
{"status": "success", "recipe": "llamacpp", "backend": "cpu"}
```

#### Cloud

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

**Response:** `200 OK`

```json
{"status": "success", "backend": "cloud", "provider": "fireworks", "models_evicted": 0}
```

### Response format

#### Local backend

A local backend removed.

- `status` - Always `success`.
- `recipe` - The recipe from the request.
- `backend` - The backend from the request.

#### Cloud provider

A cloud provider removed.

- `status` - Always `success`.
- `backend` - Always `cloud`.
- `provider` - The provider that was removed.
- `models_evicted` - Discovered models removed from the catalog.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | A cloud uninstall has no `provider`, or an empty one |
| `400` |  | A local request has no `recipe` |
| `404` |  | `provider` was not installed with [`POST /v1/install`](#post-v1install) |
| `500` |  | The request fails unexpectedly |
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
| `allow_insecure_http` | No | Must be `true` to send an API key to a provider whose base URL is `http://`; the opt-in is saved with the provider. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{
  "provider": "fireworks",
  "allow_insecure_http": false,
  "auth_state": {"env_var_set": false, "runtime_key_set": true},
  "models_discovered": 0
}
```

### Response format

- `provider` - The provider the key was set for.
- `allow_insecure_http` - Whether the provider may receive keys over `http://`, after this request.
- `auth_state` - Which API keys are set for the provider.
  - `env_var_set` - Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`.
  - `runtime_key_set` - Whether a key is held in `lemond`'s memory.
- `models_discovered` - Chat models discovered with the key; `0` when discovery fails.
- `warnings` - Present when the provider's base URL is `http://`.
- `warning` - The warnings joined into one string, for older clients.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` | `insecure_http_requires_opt_in` | The provider's base URL is `http://`, an API key would be sent to it, and neither this request nor an earlier install sets `allow_insecure_http` |
| `404` |  | `provider` was not installed with [`POST /v1/install`](#post-v1install) |
| `409` |  | `LEMONADE_<PROVIDER>_API_KEY` is set and takes precedence, so the key is not stored; `error.type` is `auth_conflict` and `error.env_var` names the variable |
| `500` |  | The request fails unexpectedly |
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

### Example

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

**Response:** `200 OK`

```json
{
  "provider": "fireworks",
  "cleared_runtime_key": true,
  "auth_state": {"env_var_set": false, "runtime_key_set": false}
}
```

### Response format

- `provider` - The provider whose key was cleared.
- `cleared_runtime_key` - `false` when no in-memory key was present, for example when the only key came from the environment variable.
- `auth_state` - Which API keys are set for the provider, after the clear.
  - `env_var_set` - Whether `LEMONADE_<PROVIDER>_API_KEY` is set for `lemond`.
  - `runtime_key_set` - Whether a key is held in `lemond`'s memory.

### Errors

| Status | When |
|--------|------|
| `500` | The request fails unexpectedly |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/live"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/live
    ```

**Response:** `200 OK`

```json
{"status": "ok"}
```

### Response format

- `status` - Always `ok`.
<!-- END GENERATED: lemonade.live -->

## Job Engine API

The endpoints below create and manage persistent server-side jobs.

<!-- BEGIN GENERATED: lemonade.jobs_create -->
## `POST /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Creates a job: a sequence of server operations that runs in the background, passing data between steps and branching on results. A job survives client disconnects and server restarts (a restart leaves an active job interrupted), and can be paused, interrupted, resumed, read and deleted.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `name` | No | Display name. Defaults to `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | The steps, in order, at least one; see [Recipe: steps](../dev/job-system.md#recipe-steps). Required unless `definition` holds them. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `definition` | No | Alternative to `steps`: an object whose `steps` holds the steps. Takes precedence over `steps`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `inputs` | No | Values the steps read as `context.inputs`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The job store holds 50 jobs: a new job evicts the oldest finished one.

A job with any `load`, `unload` or `chat` step holds the Router's exclusive slot for as long as it runs, so normal traffic queues behind it.

[`docs/dev/job-system.md`](../dev/job-system.md) specifies the step schema, op set and lifecycle, and [`docs/dev/job-expression-language.md`](../dev/job-expression-language.md) the `when`/`branch` expression grammar.

### Example

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

**Response:** `202 Accepted`

```json
{"id": "job-20261007-171503-000001"}
```

### Response format

- `id` - The job's id.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | The step graph is invalid, for example no steps, a duplicate step id or an unknown `op` |
| `429` |  | All 50 jobs in the store are still active or resumable |
<!-- END GENERATED: lemonade.jobs_create -->

<!-- BEGIN GENERATED: lemonade.jobs_list -->
## `GET /v1/jobs`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Lists a summary of every job, active or finished.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/jobs"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs
    ```

**Response:** `200 OK`

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
    ... 1 more
  ]
}
```

### Response format

- `jobs` - Every job not deleted or evicted, newest first.
  - `id` - The job's id.
  - `name` - The display name the job was created with.
  - `status` - The job's state: `queued`, `running`, `paused`, `interrupted`, `completed` or `failed`.
  - `created_at` - When the job was created, as an ISO 8601 UTC time.
  - `finished_at` - When the job finished, present once it has.
  - `progress` - How far through its steps the job is.
    - `cursor` - Id of the current or next step; empty once the job has run its last step.
    - `completed` - Steps that need no more work: completed, skipped, or failed with the failure handled.
    - `step_count` - How many steps the job has.
  - `summary` - Summary text that Lemonade never writes itself: present only when the job's entry in `jobs.json`, in the Lemonade config directory, has one.
  - `error` - Why the job failed or was interrupted, present when known.
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

### Example

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

**Response:** `200 OK`

```json
{"status": "pausing"}
```

### Response format

- `status` - Always `pausing`.

### Errors

| Status | When |
|--------|------|
| `404` | `id` names no job, a deleted one, or one that is not queued or running |
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

### Example

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

**Response:** `200 OK`

```json
{"status": "interrupting"}
```

### Response format

- `status` - Always `interrupting`.

### Errors

| Status | When |
|--------|------|
| `404` | `id` names no job, a deleted one, or one that is not queued or running |
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

### Example

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

**Response:** `200 OK`

```json
{"status": "resuming"}
```

### Response format

- `status` - Always `resuming`.

### Errors

| Status | When |
|--------|------|
| `404` | `id` names no job, a deleted one, or one that is not paused or interrupted |
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

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/jobs/job-20261007-171503-000007"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/jobs/job-20261007-171503-000007
    ```

**Response:** `200 OK`

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

### Response format

- `id` - The job's id.
- `name` - The display name the job was created with.
- `status` - The job's state: `queued`, `running`, `paused`, `interrupted`, `completed` or `failed`.
- `inputs` - The `inputs` the job was created with.
- `context` - Each completed step's output under its id, the extracted keys, and inputs.
- `steps` - The job's steps, in order.
  - `id` - The step's id.
  - `op` - The operation the step runs.
  - `params` - The step's parameters.
  - `status` - The step's state: `pending`, `running`, `completed`, `failed` or `skipped`.
  - `duration_ms` - How long the step ran, in milliseconds.
  - `error` - Why the step failed, present after a failure.
  - `output` - What the step returned, present once it has output.
- `cursor` - Id of the current or next step; empty once the job has run its last step.
- `created_at` - When the job was created, as an ISO 8601 UTC time.
- `started_at` - When the job first ran, present once it has.
- `finished_at` - When the job finished, present once it has.
- `summary` - Summary text that Lemonade never writes itself: present only when the job's entry in `jobs.json`, in the Lemonade config directory, has one.
- `error` - Why the job failed or was interrupted, present when known.

### Errors

| Status | When |
|--------|------|
| `404` | `id` names no job, or a deleted one |
<!-- END GENERATED: lemonade.jobs_get -->

<!-- BEGIN GENERATED: lemonade.jobs_delete -->
## `DELETE /v1/jobs/{id}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Removes a job at once, even a running one, which is interrupted. The models a running or paused job loaded are unloaded in the background; a finished job's models stay loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Job id, from [`POST /v1/jobs`](#post-v1jobs). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{"status": "deleted"}
```

### Response format

- `status` - Always `deleted`.

### Errors

| Status | When |
|--------|------|
| `404` | `id` names no job, or a deleted one |
<!-- END GENERATED: lemonade.jobs_delete -->
