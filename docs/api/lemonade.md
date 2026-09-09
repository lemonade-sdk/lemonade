# Lemonade API

We have designed a set of Lemonade-specific endpoints to enable client applications by extending the existing cloud-focused APIs (e.g., OpenAI). These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:

- Download models at setup time.
- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Unload models to save memory space.
- Understand system resources and state to make dynamic choices.

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/pull`](#post-v1pull) | Install a model |
| `POST` | [`/v1/models/register`](#post-v1modelsregister) | Register or update a user model definition without downloading it |
| `POST` | [`/v1/routing/validate`](#post-v1routingvalidate) | Evaluate an ad-hoc routing policy against a prompt without registering it |
| `GET` | [`/v1/downloads`](#get-v1downloads) | List server-owned model download jobs |
| `POST` | [`/v1/downloads/control`](#post-v1downloadscontrol) | Pause, cancel, or remove server-owned model download jobs |
| `GET` | [`/v1/registry/search`](#get-v1registrysearch) | Search Hugging Face or ModelScope for model repositories |
| `GET` | [`/v1/pull/variants`](#get-v1pullvariants) | Enumerate GGUF variants for a Hugging Face checkpoint |
| `POST` | [`/v1/delete`](#post-v1delete) | Delete a model |
| `POST` | [`/v1/load`](#post-v1load) | Load a model |
| `POST` | [`/v1/unload`](#post-v1unload) | Unload a model |
| `POST` | [`/v1/audio/generations`](#post-v1audiogenerations) | Generate audio (music or sound effects) from a text prompt |
| `POST` | [`/v1/classify`](#post-v1classify) | Classify input text with an encoder classifier (label scores) |
| `POST` | [`/v1/3d/generations`](#post-v13dgenerations) | Generate a textured 3D mesh (GLB) from an image |
| `POST` | [`/v1/models/check-updates`](#post-v1modelscheck-updates) | Manually check downloaded models for upstream updates |
| `GET` | [`/v1/models/{id}/files`](#get-v1modelsidfiles) | List resolved local file metadata for one model |
| `GET` | [`/v1/models/{id}/options`](#get-v1modelsidoptions) | Read a model's saved, effective, and default recipe options |
| `POST` | [`/v1/models/{id}/options`](#post-v1modelsidoptions) | Save recipe options for a model without loading it |
| `DELETE` | [`/v1/models/{id}/options`](#delete-v1modelsidoptions) | Reset a model's recipe options to defaults |
| `GET` | [`/v1/health`](#get-v1health) | Check server status, such as models loaded |
| `GET` | [`/v1/stats`](#get-v1stats) | Performance statistics from the last request |
| `GET` | [`/v1/system-stats`](#get-v1system-stats) | Current host resource usage |
| `GET` | [`/v1/system-info`](#get-v1system-info) | System information and device enumeration |
| `POST` | [`/v1/install`](#post-v1install) | Install or update a backend, or register a cloud provider |
| `POST` | [`/v1/install/dry-run`](#post-v1installdry-run) | Resolve backend install metadata without downloading the backend asset |
| `POST` | [`/v1/uninstall`](#post-v1uninstall) | Remove a backend or cloud provider |
| `POST` | [`/v1/cloud/auth`](#post-v1cloudauth) | Set an in-memory API key for a cloud provider |
| `DELETE` | [`/v1/cloud/auth/{provider}`](#delete-v1cloudauthprovider) | Clear the in-memory API key for a cloud provider |
| `WS` | [`/logs/stream`](#log-streaming-api-websocket) | Log Streaming |
| `GET` | [`/live`](#get-live) | Check server liveness for load balancers and orchestrators |
| `GET` | [`/metrics`](#get-metrics) | Prometheus metrics scrape endpoint |
| `POST` | [`/internal/telemetry/flush`](#post-internaltelemetryflush) | Force-flush all queued telemetry trace spans |
| `GET` | [`/internal/aliases`](#get-internalaliases) | List all active model aliases |
| `POST` | [`/internal/aliases`](#post-internalaliases) | Create or update a model alias |
| `DELETE` | [`/internal/aliases/{alias}`](#delete-internalaliasesalias) | Remove a model alias |

## `POST /v1/classify`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Run an encoder text-classifier (PII, prompt-safety, domain, etc.) on an input string and return per-label scores in `[0, 1]`. The target model must use the `onnxruntime` recipe. Both sequence-classification (one label set) and token-classification (aggregated span labels) models are supported.

**Supported architectures:** single-sequence encoder families — BERT, DistilBERT, RoBERTa, XLM-RoBERTa, DeBERTa (v1/v2), ELECTRA, ALBERT, CamemBERT. A stock `optimum-cli export onnx` directory of one of these works as-is.

A servable model directory is `model.onnx` + `tokenizer.json` + `config.json`. The `config.json` is **always required**: it declares the architecture, which is checked against the list above so an unsupported family (e.g. XLNet, which uses different segment/special-token conventions) is **rejected at load time** rather than served with wrong scores. The output contract (labels, normalization, token budget) is read from that same config; an optional `manifest.json` overrides it but does not replace the config. Without a manifest, inference assumes **single-label softmax**; a multi-label (sigmoid) model must declare `problem_type: multi_label_classification` in its config or ship a `manifest.json`.

This endpoint provides the classification capability that the router's `classifier` condition type will consume; the live routing-policy wiring is tracked in [#2384](https://github.com/lemonade-sdk/lemonade/issues/2384).

The endpoint is available at:

- `/v1/classify`
- `/api/v1/classify`
- `/v0/classify`
- `/api/v0/classify`

### Parameters

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `model` | string | yes* | Classifier model id (a model with the `onnxruntime` recipe). *Optional when a classification model is already loaded; the loaded model is used and echoed in the response. |
| `input` | string | yes | Text to classify. `text` is accepted as an alias. |
| `top_k` | integer | no | Return only the highest-scoring `k` labels. |

### Example request

```bash
curl -X POST http://localhost:13305/v1/classify   -H "Content-Type: application/json"   -d '{"model": "Phishing-Email-Detection-ONNX", "input": "Please verify your account at http://secure-login.example now."}'
```

### Response format

```json
{
  "object": "classification",
  "model": "Phishing-Email-Detection-ONNX",
  "labels": {
    "LABEL_1": 0.982,
    "LABEL_0": 0.011,
    "LABEL_2": 0.005,
    "LABEL_3": 0.002
  }
}
```

Label names come from the model's `id2label` — from `config.json`, or from `manifest.json` when one is present to override it; some upstream models only declare generic `LABEL_<n>` names — see the model card for their meaning.

Malformed requests (invalid JSON, missing `input`/`text`, non-string fields, non-positive `top_k`) return `400` with an `error` object before any model is loaded.

## Routing (`collection.router`)
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Naming a registered `collection.router` model in the `model` field of a
`chat/completions` or `completions` request triggers the routing engine: the
server picks a candidate by the policy's first-matching rule (fail-open to
`default_model`) and forwards the request to it. No dedicated endpoint or `"auto"`
model is involved.

The decision is reported on the response:

- Header **`x-lemonade-route`** — the matched rule id, or `default`.
- Request field **`route_trace: true`** adds an **`x_lemonade_route`** object to the
  response body: `{ route_to, matched_rule, default_used, outputs, trace[] }`
  (`route_to` is the candidate that answered). For streaming responses it is
  attached to the first SSE event.

See [Router Policies](../dev/router-policy.md) for authoring the policy.

## `POST /v1/routing/validate`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Evaluate a routing policy document against a prompt and return the decision the
engine would make, without registering the policy or dispatching the user
request to the selected candidate. This is the endpoint behind the Router
Builder's **Test Prompt** tab: it lets a policy be iterated on before it is
attached to a `collection.router` model.

The endpoint performs parser-level structural policy validation: every
`candidates` entry, `default_model`, rule `route_to`, and classifier model must
be listed in `components`. It does not consult the live model registry:
component names are accepted as-is, so a policy can be tested before its
candidates are downloaded. Because names and component model types are not
resolved through the registry, registration-time registry checks (for example,
whether a `semantic_similarity` model can embed or a `classifier` model can
classify/chat) are not performed by this endpoint.

Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, …)
are evaluated locally. Model-backed conditions (`semantic_similarity`,
`classifier`, and `llm`, including `routing.router`) may load and run their
referenced models. A model-evaluation failure is handled by the classifier's
`on_error` policy (`match_false` by default), so routing normally continues to a
later rule or falls through to `default_model` rather than treating the policy
as invalid.

The endpoint is available at:

- `/v1/routing/validate`
- `/api/v1/routing/validate`
- `/v0/routing/validate`
- `/api/v0/routing/validate`

### Parameters

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `policy` | object | yes | A `collection.router` policy document. `model_name` is accepted but is not required for validation. See [Router Policies](../dev/router-policy.md). |
| `prompt` | string | no | The prompt text to route. Defaults to `""`, which still exercises `min_chars` (0 chars) and any prompt-independent rules. |
| `has_images` | boolean | no | Simulate a request carrying image input. Default `false`. |
| `has_tools` | boolean | no | Simulate a request carrying tool definitions. Default `false`. |
| `metadata` | object | no | String-valued metadata pairs matched by `metadata` conditions. |

### Example request

```bash
curl -X POST http://localhost:13305/api/v1/routing/validate \
     -H "Content-Type: application/json" \
     -d '{
           "policy": {
             "version": "1",
             "recipe": "collection.router",
             "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
             "routing": {
               "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
               "default_model": "Qwen3-8B-GGUF",
               "rules": [
                 {
                   "id": "code-to-big",
                   "match": {"keywords_any": ["def ", "function", "compile"]},
                   "route_to": "vllm.qwen3-32b"
                 }
               ]
             }
           },
           "prompt": "please write a def to reverse a list"
         }'
```

### Response format

```json
{
  "decision": {
    "version": "1",
    "route_to": "vllm.qwen3-32b",
    "matched_rule": "code-to-big",
    "default_used": false,
    "outputs": {},
    "trace": [
      { "condition": "keywords_any", "result": true }
    ]
  },
  "normalized_policy": {
    "version": "1",
    "recipe": "collection.router",
    "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
    "routing": {
      "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
      "default_model": "Qwen3-8B-GGUF",
      "rules": [
        {
          "id": "code-to-big",
          "match": {"keywords_any": ["def ", "function", "compile"]},
          "route_to": "vllm.qwen3-32b"
        }
      ]
    }
  }
}
```

`decision` has the same shape as the `x_lemonade_route` object a routed
completion returns with `route_trace: true`, and the trace is always included
here. When no rule matches, `matched_rule` is empty, `default_used` is `true`,
and `route_to` is the policy's `default_model`.

`normalized_policy` echoes the policy as it was actually evaluated. The policy
above uses explicit `routing.rules`, so it comes back unchanged. The field earns
its place when a policy uses the `routing.router` shorthand: that sugar is
desugared into an explicit `llm` classifier plus one identity rule per
candidate, so a `routing` block authored as:

```json
{
  "router": {
    "type": "llm",
    "model": "Qwen3-8B-GGUF",
    "prompt": "Pick the best model for this request."
  },
  "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
  "default_model": "Qwen3-8B-GGUF"
}
```

is echoed back with `router` removed and synthesized `classifiers`/`rules`:

```json
{
  "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
  "default_model": "Qwen3-8B-GGUF",
  "classifiers": [
    {
      "id": "__router",
      "type": "llm",
      "model": "Qwen3-8B-GGUF",
      "prompt": "Pick the best model for this request.",
      "labels": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"]
    }
  ],
  "rules": [
    {
      "id": "__route_0",
      "match": {"classifier": "__router", "label": "Qwen3-8B-GGUF", "min_score": 1.0},
      "route_to": "Qwen3-8B-GGUF"
    },
    {
      "id": "__route_1",
      "match": {"classifier": "__router", "label": "vllm.qwen3-32b", "min_score": 1.0},
      "route_to": "vllm.qwen3-32b"
    }
  ]
}
```

Match `decision.matched_rule` against this document rather than the one you
sent — a policy authored with only `routing.router` has no `routing.rules` of
its own, only the synthesized `__route_0`, `__route_1`, … rules shown here.

### Error responses

| Status | Condition |
|--------|-----------|
| `400` | Body is not valid JSON, `policy` is missing or not an object, `prompt` is not a string, `has_images`/`has_tools` are not booleans, or `metadata` is not an object of string values. |
| `400` | The policy document is invalid or internally inconsistent; the `error` field is prefixed with `Invalid routing policy:`. |

## `POST /v1/models/check-updates`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly checks downloaded Hugging Face-backed models for newer upstream
commits. This is the manual counterpart to the startup update check and works
even when `auto_check_model_updates=false`.

Full offline mode remains authoritative: when `offline=true`, this endpoint
returns HTTP 409 and does not make network requests.

### Example request

```bash
curl -X POST http://localhost:13305/v1/models/check-updates
```

The same action is available from the CLI:

```bash
lemonade check-updates
```

### Response format

```json
{
  "status": "success",
  "updates_available": 2,
  "models": [
    "Qwen3-4B-GGUF",
    "Whisper-Tiny"
  ]
}
```

The endpoint is available at:

- `/v1/models/check-updates`
- `/api/v1/models/check-updates`
- `/v0/models/check-updates`
- `/api/v0/models/check-updates`

## `GET /v1/models/{id}/files`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

List resolved local file metadata for a single model. This endpoint is intended for model-detail UIs such as the Files tab. It is per-model inventory, not system or drive storage accounting.

The endpoint is available at:

- `/v1/models/{id}/files`
- `/api/v1/models/{id}/files`
- `/v0/models/{id}/files`
- `/api/v0/models/{id}/files`

By default, the response does not include absolute filesystem paths. Trusted local clients that need paths for native UI actions can request them explicitly with `?include_paths=true`. Absolute paths may reveal local usernames and cache layout, so clients should only request them when that disclosure is acceptable.

### Example request

```bash
curl http://localhost:13305/v1/models/Qwen3-4B/files
```

### Response format

```json
{
  "model_id": "Qwen3-4B",
  "files": [
    {
      "name": "model.gguf",
      "role": "main",
      "size_bytes": 123456789,
      "exists": true
    },
    {
      "name": "mmproj.gguf",
      "role": "mmproj",
      "size_bytes": 12345678,
      "exists": true
    }
  ]
}
```

### Optional path disclosure

```bash
curl 'http://localhost:13305/v1/models/Qwen3-4B/files?include_paths=true'
```

When `include_paths=true` is supplied, each file entry also includes `path`:

```json
{
  "name": "model.gguf",
  "path": "/abs/path/model.gguf",
  "role": "main",
  "size_bytes": 123456789,
  "exists": true
}
```

### Fields

| Field | Description |
|-------|-------------|
| `model_id` | Public model ID for the requested model. |
| `files` | Array of resolved model files known to the registry. |
| `files[].name` | Base filename from the resolved path. |
| `files[].path` | Absolute resolved path on the local system. Only included when `include_paths=true`; privacy-sensitive. |
| `files[].role` | Checkpoint role, for example `main`, `mmproj`, or another recipe-specific role. |
| `files[].size_bytes` | File size in bytes. Directories are summed recursively. Missing files report `0`. |
| `files[].exists` | Whether the resolved path currently exists on disk. |

## `GET /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Read a model's recipe options, separated by layer, without loading it. With `POST` and `DELETE` on the same path, this manages per-model options independently of [`/v1/load`](#post-v1load).

### Example request

```bash
curl http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options
```

### Response format

`effective` is the exact request body a [`POST /v1/load`](#post-v1load) for this model uses right now, with every option the recipe accepts resolved through the full priority chain. `defaults` is what a reset model would get. For `llamacpp`, with `--no-mmap` saved and the context size left automatic:

```json
{
  "model_name": "Qwen3-0.6B-GGUF",
  "recipe": "llamacpp",
  "saved": {
    "llamacpp_args": "--no-mmap"
  },
  "effective": {
    "auto_evict": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "--no-mmap",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-0.6B-GGUF"
  },
  "defaults": {
    "auto_evict": null,
    "ctx_size": -1,
    "downsize_idle_timeout": 60,
    "evict_idle_timeout": 300,
    "evict_weight_factor": 1.0,
    "llamacpp_args": "",
    "llamacpp_backend": "vulkan",
    "llamacpp_device": "",
    "merge_args": true,
    "model_name": "Qwen3-0.6B-GGUF"
  },
  "resolved_ctx_size": 32768
}
```

| Field | Description |
|-------|-------------|
| `model_name` | The id from the URL. It appears again inside `effective` and `defaults` so that each one is a complete `/v1/load` body. |
| `recipe` | The recipe the option names belong to. |
| `saved` | The model's own entry in `recipe_options.json`: only what was explicitly saved, or `{}` when nothing is. It can also hold keys this endpoint does not accept, such as `pinned` written by `/v1/load`, so replay `effective` rather than `saved`. |
| `effective` | The `/v1/load` body shown above. Posting it back whole to this endpoint saves every resolved value as an override, so send only the options the user changed. |
| `defaults` | What `effective` becomes if `saved` is erased, in the same shape. A `ctx_size` of `-1` means the server picks the context size automatically. |
| `resolved_ctx_size` | The context size a load right now would use: the effective `ctx_size`, or the automatically computed size when that is `-1`. |

> Note: per-architecture defaults come from the model's GGUF metadata. For a model that has not been downloaded yet, every key is still present but carries the value it has before those defaults apply.

## `POST /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Save recipe options for a model without loading it. The request body is a flat object of the same recipe options [`/v1/load`](#post-v1load) accepts. The URL identifies the model; a `model_name` in the body is ignored.

The request merges into the model's saved entry, so keys you don't mention are left alone. `null` removes an option, and the model falls back to the next layer of the [priority chain](#post-v1load). [`DELETE`](#delete-v1modelsidoptions) removes every saved option at once.

`dry_run: true` validates and resolves the request identically but persists nothing: `effective` and `resolved_ctx_size` describe the state the save would produce, while `saved` keeps reporting the entry on disk. Use it to preview a change before committing it.

`ctx_size` takes a positive whole number, or `-1` to pin the model to automatic sizing even when the server-wide `ctx_size` is a specific number.

A `400` reports an unrecognized option name, an option from a different recipe, a value of the wrong type, or an invalid `ctx_size`, and nothing from that request is saved.

Saving never loads or reloads the model, so a model that is already running keeps its current options until it is next loaded.

> Note: `pinned` is not settable here and is omitted from `effective` and `defaults`. It belongs to [`/v1/load`](#post-v1load) and `/internal/pin`.

### Example requests

Save a context size without loading the model:

```bash
curl -X POST http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options \
  -H "Content-Type: application/json" \
  -d '{"ctx_size": 8192, "llamacpp_backend": "vulkan"}'
```

Set the context size back to automatic, leaving the backend choice saved:

```bash
curl -X POST http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options \
  -H "Content-Type: application/json" \
  -d '{"ctx_size": -1}'
```

### Response format

Same as [`GET /v1/models/{id}/options`](#get-v1modelsidoptions), reflecting the state after the write.

## `DELETE /v1/models/{id}/options`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reset a model to its defaults by erasing its `recipe_options.json` entry entirely. The model keeps the defaults that come from its registry entry and from the server's global configuration; only the user's saved overrides are removed.

### Example request

```bash
curl -X DELETE http://localhost:13305/v1/models/Qwen3-0.6B-GGUF/options
```

### Response format

Same as [`GET /v1/models/{id}/options`](#get-v1modelsidoptions), with `saved` now `{}`.

## `POST /v1/models/register`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Register or update a `user.*` model definition without downloading model files.
Use this endpoint when registration and installation are separate actions.
`POST /v1/pull` remains the install/download path and performs the same internal
registration step before downloading.

The endpoint is available at:

- `/v1/models/register`
- `/api/v1/models/register`
- `/v0/models/register`
- `/api/v0/models/register`

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | Non-empty namespaced model name under `user.*`. |
| `recipe` | Yes | Lemonade recipe associated with the model definition. |
| `checkpoint` | No | Main checkpoint, when the recipe uses one. |
| `checkpoints` | No | Named checkpoints for multi-checkpoint models. |
| `source` | No | Registry or local source. Remote values are `huggingface` or `modelscope`. |
| `labels` | No | Additional model labels. |
| `components` | No | Already-registered component model names for collection recipes. |

A checkpoint is intentionally not universally required: registration is a model
metadata operation and some present or future model types may not have local
weights. `/pull` remains the operation that attempts installation/download.

The endpoint accepts one model definition. An embedded `models` array represents
multiple definitions and remains a collection-import concern; register those
component definitions first when using this endpoint.

Example request:

```bash
curl -X POST http://localhost:13305/v1/models/register \\
  -H "Content-Type: application/json" \\
  -d '{
    "model_name": "user.Phi-4-Mini-GGUF",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M",
    "recipe": "llamacpp"
  }'
```

Example response:

```json
{
  "status": "success",
  "model_name": "Phi-4-Mini-GGUF",
  "canonical_model_name": "user.Phi-4-Mini-GGUF",
  "model": {
    "id": "Phi-4-Mini-GGUF",
    "recipe": "llamacpp",
    "downloaded": false
  }
}
```

`model_name` is the public ID exposed by `/v1/models`; `canonical_model_name` is
the stable `user.*` registration ID. Registration updates `user_models.json` and
invalidates the model cache, but does not start a model download.

## `POST /v1/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Register and install models for use with Lemonade Server.

### Parameters

The Lemonade Server built-in model registry has a collection of model names that can be pulled and loaded. The `pull` endpoint can install any registered model, and it can also register-then-install any model available on Hugging Face.

**Common Parameters**

| Parameter | Required | Description |
|-----------|----------|-------------|
| `stream` | No | If `true`, returns Server-Sent Events (SSE) with download progress. Defaults to `false`. |
| `subscribe` | No | Only applies when `stream=true`. If `false`, the server starts a background model download job and returns a JSON snapshot immediately instead of keeping the HTTP response subscribed to SSE progress. Defaults to `true` for backwards compatibility. |

**Install a Model that is Already Registered**

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to install. |

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
  "message":"Installed model: Qwen3-0.6B-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

**Register and Install a Model**

Registration will place an entry for that model in the `user_models.json` file, which is located in the user's Lemonade config directory (default: `~/.config/lemonade`). Then, the model will be installed. Once the model is registered and installed, it will show up in the `models` endpoint alongside the built-in models and can be loaded.

The `recipe` field defines which software framework and device will be used to load and run the model.

> Note: the `model_name` for registering a new model must use the `user` namespace, to prevent collisions with built-in models. For example, `user.Phi-4-Mini-GGUF`.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | Namespaced [Lemonade Server model name](https://lemonade-server.ai/models.html) to register and install. |
| `recipe` | Yes | Lemonade API recipe to load the model with. |
| `checkpoint` | Yes`*` | HuggingFace "main" checkpoint to install. |
| `checkpoints` | No | HuggingFace checkpoints to install, for multi-checkpoint models. |
| `reasoning` | No | Whether the model is a reasoning model, like DeepSeek (default: false). Adds 'reasoning' label. |
| `vision` | No | Whether the model has vision capabilities for processing images (default: false). Adds 'vision' label. |
| `embedding` | No | Whether the model is an embedding model (default: false). Adds 'embeddings' label. |
| `reranking` | No | Whether the model is a reranking model (default: false). Adds 'reranking' label. |
| `mmproj` | No | Multimodal Projector (mmproj) file to use for vision models. |

A model definition requires at least a `main` checkpoint. This can be either
be specified with the `checkpoint` parameter, or a `main` key in the
`checkpoints` dict.

Each backend serves a fixed set of [deployment modes](openai.md#model-labels),
and a model deploys in exactly one of them. Naming a mode the recipe cannot
serve, or naming two — whether through `labels` or through the `embedding` /
`reranking` parameters — is rejected with `400` and nothing is registered:

```bash
curl -X POST http://localhost:8000/api/v1/pull \
  -H "Content-Type: application/json" \
  -d '{"model_name": "user.Clf", "recipe": "llamacpp",
       "checkpoint": "example/model:Q4_K_M", "labels": ["classification"]}'
```

```json
{"error": "Model 'user.Clf': recipe 'llamacpp' cannot serve 'classification'. It serves 'chat', 'embeddings', 'reranking'. Omit the label to deploy as 'chat'."}
```

```bash
curl -X POST http://localhost:8000/api/v1/pull \
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
  "message":"Installed model: user.Phi-4-Mini-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

**Register an Omni-Model**

An omni collection is a collection type that bundles several models into a single entry that can be loaded, pulled, or deleted as a unit. Use `recipe: "collection.omni"` with a `components` array instead of `checkpoint`.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | Namespaced model name, e.g. `user.MyKit`. |
| `recipe` | Yes | Must be `"collection.omni"`. |
| `components` | Yes | Ordered, non-empty array of model names. Each component must be a regular model. |
| `models` | No | Ordered array of full model definitions, one per `components` entry (the same fields as single-model registration, keyed by `model_name`). When present, component names that are not yet registered are registered from these definitions; names that already exist keep their local definition. When absent, every `components` entry must already exist in the registry (built-in or a previously registered `user.*` model). |

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

### Import an Exported Model File

Files written by `lemonade export` (and the desktop app's Export button) are import-ready
`/v1/pull` request bodies — POST the file contents verbatim to register and install the model.
This works for regular models and collections alike; exported collection files additionally
carry `components` plus a `models` array embedding each component's definition (see the
`models` parameter above). For the file format and the export/import/Hugging Face workflows,
see [Share a collection](../guide/configuration/custom-models.md#share-a-collection-export-import-and-hugging-face).

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

## `GET /v1/downloads`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

List server-owned model download jobs that were started with `POST /v1/pull` using `stream=true` and `subscribe=false`.

This endpoint is intended for clients that need to restore download-manager state after a reload or reconnect. Active, paused, cancelled, and errored jobs remain visible until the client removes them. Completed jobs remain visible briefly so clients can observe completion and refresh model state.

### Example request

```bash
curl http://localhost:13305/v1/downloads
```

### Response format

```json
[
  {
    "id": "model:Qwen3-0.6B-GGUF",
    "type": "model",
    "model_name": "Qwen3-0.6B-GGUF",
    "status": "downloading",
    "running": true,
    "file": "model.gguf",
    "file_index": 1,
    "total_files": 2,
    "bytes_downloaded": 1073741824,
    "bytes_total": 2684354560,
    "total_download_size": 2684355584,
    "bytes_previously_downloaded": 0,
    "completed_files_bytes": 0,
    "cumulative_bytes_downloaded": 1073741824,
    "overall_bytes_downloaded": 1073741824,
    "percent": 40,
    "complete": false
  }
]
```

### Download job fields

| Field | Description |
|-------|-------------|
| `id` | Stable download id. Model downloads use `model:<model_name>`. |
| `type` | Download type. Currently `model` for server-owned jobs. |
| `model_name` | Lemonade model name associated with the job. |
| `status` | Current state: `downloading`, `paused`, `cancelled`, `completed`, or `error`. |
| `running` | Whether the download worker is still active. A terminal-looking status may still have `running=true` while the worker is releasing resources. |
| `file`, `file_index`, `total_files` | Current file progress within the download. |
| `bytes_downloaded`, `bytes_total`, `percent` | Current-file byte progress as reported by the downloader. |
| `total_download_size` | Total expected bytes across all files when known. |
| `bytes_previously_downloaded` | Bytes already present on disk for the current file when resuming or skipping existing data. |
| `completed_files_bytes` | Bytes from files completed before the current file. |
| `cumulative_bytes_downloaded`, `overall_bytes_downloaded` | Total bytes downloaded across the whole job. `overall_bytes_downloaded` is kept as a compatibility alias. |
| `complete` | `true` when the download completed successfully. |
| `error` | Error message, present only for failed jobs. |

## `POST /v1/downloads/control`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Control a server-owned model download job.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `id` | Yes | Download id returned by `POST /v1/pull` or `GET /v1/downloads`, for example `model:Qwen3-0.6B-GGUF`. |
| `action` | Yes | One of `pause`, `cancel`, or `remove`. |

### Actions

| Action | Description |
|--------|-------------|
| `pause` | Requests the worker to stop and keeps the job visible as `paused`. The worker may briefly report `running=true` while it unwinds. |
| `cancel` | Requests the worker to stop and marks the job as `cancelled`. Clients should wait for `running=false` before deleting partial files. |
| `remove` | Removes a stopped job from the server registry. If the worker is still running, the server keeps the job visible and treats the request as a cancel request until the worker stops. |

### Example request

```bash
curl -X POST http://localhost:13305/v1/downloads/control \
  -H "Content-Type: application/json" \
  -d '{
    "id": "model:Qwen3-0.6B-GGUF",
    "action": "pause"
  }'
```

### Response format

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

## `GET /v1/registry/search`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Search a remote model registry (Hugging Face or ModelScope) for repositories matching a text query. This endpoint returns **candidate repositories** based on registry metadata; it does not verify that a repository contains servable files. The desktop app's Model Manager follows up with [`/v1/pull/variants`](#get-v1pullvariants) on each candidate and only offers a download for repositories whose file listing passes that validation.

Requires network access: returns 400 with code `lemond_offline` when the server is in offline mode.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `query` | Yes | Search text, minimum 3 characters after trimming. `q` is accepted as an alias. |
| `source` | No | Registry to search: `huggingface` (default) or `modelscope`. Aliases `hf` and `ms` are accepted; the canonical name is echoed in the response. |
| `limit` | No | Maximum number of results, an integer from 1 to 50. Default 12. |
| `format` | No | The only accepted value is `gguf`. Biases search and ranking toward GGUF repositories and echoes `"format": "gguf"` in the response. |

Example request:

```bash
curl 'http://localhost:13305/v1/registry/search?source=modelscope&query=qwen&format=gguf'
```

### Response

```json
{
  "source": "modelscope",
  "query": "qwen",
  "format": "gguf",
  "total": 128,
  "results": [
    {
      "repository_id": "Qwen/Qwen2.5-3B-Instruct-GGUF",
      "display_name": "Qwen2.5-3B-Instruct-GGUF",
      "source": "modelscope",
      "repository_type": "model",
      "description": "GGUF quantizations of Qwen2.5-3B-Instruct",
      "tags": ["gguf", "chat"],
      "task": "text-generation",
      "downloads": 222500,
      "likes": 12,
      "has_gguf": true
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `source`, `query` | Echoed input (`source` canonicalized to `huggingface` or `modelscope`). |
| `format` | Present only when `format=gguf` was requested. |
| `total` | Total match count reported by the upstream registry; may exceed the number of returned results. |
| `results[]` | Up to `limit` repositories, each with `repository_id`, `display_name`, `source`, `repository_type`, `description`, `tags`, `task`, `downloads`, `likes`, and `has_gguf`. `has_gguf` is a hint derived from registry metadata, not proof of a servable model — [`/v1/pull/variants`](#get-v1pullvariants) performs the authoritative file-level validation. |

### Error responses

| Status | Cause |
|--------|-------|
| 400 | `query` shorter than 3 characters, invalid `source`, `limit`, or `format`, or the server is in offline mode (`code: lemond_offline`). |
| 429 | The upstream registry rate-limited the request. |
| 502 | Other upstream transport or parsing failures; the body includes the upstream status code when available. |

## `GET /v1/pull/variants`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Inspect a Hugging Face GGUF repository and enumerate the variants (quantizations and sharded folder groups) available for installation. Used by the `lemonade pull <owner/repo>` CLI flow and by the desktop app's model search to auto-populate the install form. The endpoint reads only public Hugging Face metadata; if the `HF_TOKEN` environment variable is set on the server, it is forwarded as a bearer token to access gated repositories.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `checkpoint` | Yes | Hugging Face repo id, e.g. `unsloth/Qwen3-8B-GGUF`. Passed as a query string. |

Example request:

```bash
curl 'http://localhost:13305/v1/pull/variants?checkpoint=unsloth/Qwen3-8B-GGUF'
```

### Response

```json
{
  "checkpoint": "unsloth/Qwen3-8B-GGUF",
  "recipe": "llamacpp",
  "suggested_name": "Qwen3-8B-GGUF",
  "suggested_labels": ["vision"],
  "mmproj_files": ["mmproj-model-f16.gguf"],
  "variants": [
    {
      "name": "Q4_K_M",
      "primary_file": "Qwen3-8B-Q4_K_M.gguf",
      "files": ["Qwen3-8B-Q4_K_M.gguf"],
      "sharded": false,
      "size_bytes": 4920000000
    },
    {
      "name": "Q8_0",
      "primary_file": "Q8_0/Qwen3-8B-Q8_0-00001-of-00002.gguf",
      "files": ["Q8_0/Qwen3-8B-Q8_0-00001-of-00002.gguf", "Q8_0/Qwen3-8B-Q8_0-00002-of-00002.gguf"],
      "sharded": true,
      "size_bytes": 8500000000
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `checkpoint` | Echoed input. |
| `recipe` | Suggested recipe (always `llamacpp` today; future expansion may return other values). |
| `suggested_name` | Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name. |
| `suggested_labels` | Inferred labels — `vision` if any `mmproj-*.gguf` files exist, plus `embeddings`/`reranking` if those substrings appear in the repo id. |
| `mmproj_files` | Bare filenames of `mmproj-*.gguf` files in the repo; the first one should be passed as `mmproj` to `/v1/pull` for vision models. |
| `variants[]` | Top quantizations for the repo, capped at 5. Each entry has `name` (e.g. `Q4_K_M`, `UD-Q4_K_XL`), `primary_file`, `files`, `sharded`, and `size_bytes` (from the HF `?blobs=true` listing). Ranked by frequency of use in `server_models.json` (`Q4_K_M`, `UD-Q4_K_XL`, `Q8_0`, `Q4_0` first, everything else sorted lexicographically). The CLI `lemonade pull` menu adds a free-text "Other" option for quants outside the top 5. |

### Error responses

| Status | Cause |
|--------|-------|
| 400 | `checkpoint` query parameter missing or malformed (must contain `/`). |
| 404 | Hugging Face returned 404 for the checkpoint. |
| 500 | Other transport or parsing failures; the response body contains an `error` message. |

## `POST /v1/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Delete a model by removing it from local storage. If the model is currently loaded, it will be unloaded first.

> Note: deleting a collection (`recipe: "collection.omni"`) removes only the collection entry from `user_models.json`; its components stay on disk. Delete the components individually if you want to free their disk space.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](https://lemonade-server.ai/models.html) to delete. |

Example request:

```bash
curl -X POST http://localhost:13305/v1/delete \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Deleted model: Qwen3-0.6B-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

## `POST /v1/load`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly load a registered model into memory. This is useful to ensure that the model is loaded before you make a request. Installs the model if necessary.

> Note: loading a collection (`recipe: "collection.omni"`) loads each of its components in turn. Per-model options like `ctx_size` or `llamacpp_backend` are not forwarded to components — set them on each component's own `recipe_options.json` entry instead.

Recipe option fields on `/v1/load` have three-state semantics. Omitting a field keeps using its saved per-model value. Passing explicit `null` ignores only that saved key for this load and falls through to the lower default layers without changing `recipe_options.json`. Passing a concrete value overrides the saved value. For `*_args`, a concrete value replaces the model/architecture args scope for that load; backend/machine args remain only when `merge_args` is true. `ctx_size: -1` is a concrete value meaning automatic context sizing, not a tombstone. With `save_options: true`, concrete values are persisted as usual while a `null` tombstone preserves the existing saved value for that key.

### Parameters

| Parameter | Required | Applies to | Description |
|-----------|----------|------------|-------------|
| `model_name` | Yes | All | [Lemonade Server model name](https://lemonade-server.ai/models.html) to load. |
| `pinned` | No | All | Boolean. If true, pins the loaded model to prevent LRU eviction. Defaults to `false`. |
| `save_options` | No | All | Boolean. If true, saves recipe options to `recipe_options.json`. Any previously stored value for `model_name` is replaced. To save options without loading, or to change one option without resending the rest, use [`POST /v1/models/{id}/options`](#post-v1modelsidoptions) instead. |
| `ctx_size` | No | llamacpp, flm, ryzenai-llm | Context size for the model. Overrides the default value. Pass `-1` to size it automatically instead of using a saved value; omit it to use the saved value. |
| `llamacpp_backend` | No | llamacpp | LlamaCpp backend to use (`vulkan`, `rocm`, `metal` or `cpu`). |
| `llamacpp_args` | No | llamacpp | Custom arguments to pass to llama-server. The following are NOT allowed: `-m`, `--port`, `--ctx-size`, `-ngl`, `--jinja`, `--mmproj`, `--embeddings`, `--reranking`. |
| `whispercpp_backend` | No | whispercpp | WhisperCpp backend: `npu` or `cpu` on Windows; `cpu` or `vulkan` on Linux. Default is `npu` if supported. |
| `whispercpp_args` | No | whispercpp | Custom arguments to pass to whisper-server. The following are NOT allowed: `-m`, `--model`, `--port`. Example: `--convert`. |
| `steps` | No | sd-cpp | Number of inference steps for image generation. Default: 20. |
| `cfg_scale` | No | sd-cpp | Classifier-free guidance scale for image generation. Default: 7.0. |
| `width` | No | sd-cpp | Image width in pixels. Default: 512. |
| `height` | No | sd-cpp | Image height in pixels. Default: 512. |
| `merge_args` | No | All | Boolean. If true (default), backend/machine `*_args` are inherited; concrete request `*_args` replace model/architecture args while keeping backend args. If false, no inherited custom args or overridable runtime defaults are applied. |

**Setting Priority:**

When loading a model, settings are applied in this priority order:
1. Values explicitly passed in the `load` request (highest priority)
2. Per-model values configurable in `recipe_options.json` (see below for details)
3. Values from environment variables or server startup arguments (see [Server Configuration](../guide/configuration/README.md))
4. Default hardcoded values in `lemond` (lowest priority)


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
    "llamacpp_args": "--flash-attn on --no-mmap"
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
    "llamacpp_args": "--no-context-shift --no-mmap",
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

### Response format

```json
{
  "status":"success",
  "message":"Loaded model: Qwen3-0.6B-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

## `POST /v1/unload`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly unload a model from memory. This is useful to free up memory while still leaving the server process running (which takes minimal resources but a few seconds to start).

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | No | Name of the specific model to unload. If not provided, all loaded models will be unloaded. |

### Example requests

Unload a specific model:

```bash
curl -X POST http://localhost:13305/v1/unload \
  -H "Content-Type: application/json" \
  -d '{"model_name": "Qwen3-0.6B-GGUF"}'
```

Unload all models:

```bash
curl -X POST http://localhost:13305/v1/unload
```

### Response format

Success response:

```json
{
  "status": "success",
  "message": "Model unloaded successfully"
}
```

Error response (model not found):

```json
{
  "status": "error",
  "message": "Model not found: Qwen3-0.6B-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.



## `POST /v1/audio/generations`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Audio Generation API. You provide a text prompt and receive a generated audio clip. The loaded model decides the kind of audio: music with ACE-Step models (e.g. `ACE-Step-Music`), sound effects with ThinkSound models (e.g. `ThinkSound-SFX`).

This endpoint is not part of the OpenAI API (OpenAI's audio endpoints cover speech and transcription only), so it is a Lemonade-specific extension.

> **Performance:** generation runs on the GPU (Vulkan, ROCm, or CUDA) and takes from seconds (short sound effects) to minutes (full-length music) depending on duration and hardware.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model` | Yes | The audio-generation model to use (e.g., `ThinkSound-SFX`, `ACE-Step-Music`). |
| `prompt` | Yes | Text description of the music or sound effect to generate. For music, this is the style description: genre, mood, tempo, instruments, and voice. |
| `lyrics` | No | Lyrics to sing (ACE-Step only). When present and not empty, the track is generated with vocals singing these lyrics. Omitting the field, an empty string, or the sentinel `[Instrumental]` (any case) produces an instrumental track. See [Lyrics](#lyrics) below for the expected format. |
| `vocal_language` | No | BCP-47 language code of the lyrics, e.g. `en`, `fr`, `ja` (ACE-Step only). Default: `en`. |
| `duration` | No | Length of the clip in seconds. Defaults to the backend's native default. |
| `steps` | No | Number of inference steps. Lower is faster, higher can improve quality. |
| `cfg` | No | Classifier-free guidance strength (ThinkSound only). |
| `seed` | No | Random seed for reproducibility. |
| `response_format` | No | Output encoding. Only formats the backend natively produces are accepted (currently `wav`); other values are rejected with `400 Bad Request`. Default: `wav`. |

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

```bash
curl -X POST http://localhost:13305/v1/audio/generations \
  -H "Content-Type: application/json" \
  -d '{
        "model": "ThinkSound-SFX",
        "prompt": "glass shattering on a stone floor",
        "duration": 5,
        "seed": 42
      }' \
  --output clip.wav
```

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

## `POST /v1/3d/generations`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

3D Generation API. You provide an input image and receive a textured 3D mesh as a glTF-binary (`.glb`) file. Serves TRELLIS models (e.g. `TRELLIS-3D`). The input image must be PNG, JPEG, BMP, or GIF.

This endpoint is not part of the OpenAI API, so it is a Lemonade-specific extension.

> **Performance:** 3D reconstruction runs on the GPU (Vulkan, ROCm, or CUDA) and takes on the order of minutes; higher cascade resolutions take longer.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model` | Yes | The 3D-generation model to use (e.g., `TRELLIS-3D`). |
| `image` | Yes | Base64-encoded input image (optionally a `data:` URL). |
| `resolution` | No | Cascade resolution: `512`, `1024`, or `1536`. Default: `512`. |
| `bg_removal` | No | Background removal mode: `threshold` or `birefnet`. Use `birefnet` for photos with real backgrounds. |
| `uv` | No | UV atlas method: `xatlas` (default) or `box`. `xatlas` runs a full UV unwrap giving every face unique atlas space — best quality, but chart computation is superlinear in face count. `box` is a faster 6-plane projection with occlusion-aware bucket assignment and depth-tested rasterization; small texture artifacts remain possible in concave regions. |
| `seed` | No | Random seed for reproducibility. |
| `response_format` | No | Output encoding. Only formats the backend natively produces are accepted (currently `glb`); other values are rejected with `400 Bad Request`. Default: `glb`. |

### Response

On success the raw mesh bytes are returned as `model/gltf-binary`. On failure the response is JSON with an `error` object: `400` for invalid requests, `404` for unknown models, `500` when the backend reports an error, and `502` when the backend produces no output.

### Example request

```bash
curl -X POST http://localhost:13305/v1/3d/generations \
  -H "Content-Type: application/json" \
  -d "{
        \"model\": \"TRELLIS-3D\",
        \"image\": \"$(base64 -w0 input.png)\",
        \"resolution\": 512,
        \"seed\": 42
      }" \
  --output model.glb
```

## `GET /v1/health`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Check the health of the server. This endpoint returns information about loaded models.

### Parameters

This endpoint does not take any parameters.

### Example request

```bash
curl http://localhost:13305/v1/health
```

### Response format

```json
{
  "status": "ok",
  "version":"9.3.3",
  "websocket_port":9000,
  "model_loaded": "Llama-3.2-1B-Instruct-Hybrid",
  "all_models_loaded": [
    {
      "model_name": "Llama-3.2-1B-Instruct-Hybrid",
      "checkpoint": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
      "last_use": 1732123456.789,
      "type": "llm",
      "device": "gpu npu",
      "pinned": true,
      "recipe": "ryzenai-llm",
      "pid": 12345,
      "launch_command": [
        "~/.cache/lemonade/bin/ryzenai/npu/ryzenai-server.exe",
        "-m", "~/.cache/lemonade/models/Llama-3.2-1B-Instruct-Hybrid",
        "--port", "8001",
        "--ctx-size", "4096"
      ],
      "recipe_options": {
        "ctx_size": 4096
      },
      "backend_url": "http://127.0.0.1:8001/v1"
    },
    {
      "model_name": "nomic-embed-text-v1-GGUF",
      "checkpoint": "nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S",
      "last_use": 1732123450.123,
      "type": "embedding",
      "device": "gpu",
      "pinned": false,
      "recipe": "llamacpp",
      "pid": 12346,
      "launch_command": [
        "~/.cache/lemonade/bin/llamacpp/rocm-stable/llama-server.exe",
        "-m", "~/.cache/huggingface/hub/models--nomic-ai--nomic-embed-text-v1-GGUF/.../nomic-embed-text-v1.Q4_K_S.gguf",
        "--ctx-size", "8192",
        "--port", "8002",
        "--no-mmap"
      ],
      "recipe_options": {
        "ctx_size": 8192,
        "llamacpp_args": "--no-mmap",
        "llamacpp_backend": "rocm"
      },
      "backend_url": "http://127.0.0.1:8002/v1"
    }
  ],
  "pinned_models": {
    "transcription":0,
    "embedding":0,
    "image":0,
    "llm":1,
    "reranking":0,
    "tts":0
  },
  "max_models": {
    "transcription":1,
    "embedding":1,
    "image":1,
    "llm":1,
    "reranking":1,
    "tts":1
  },
  "telemetry": {
    "enabled": false
  },
  "update_check_done": true
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
  - `last_use` - Unix timestamp of last access (load or inference)
  - `type` - Model type: `"llm"`, `"embedding"`, `"reranking"`, `"transcription"`, `"image"`, or `"tts"`
  - `device` - Space-separated device list: `"cpu"`, `"gpu"`, `"npu"`, or combinations like `"gpu npu"`
  - `pinned` - Boolean indicating if the model is currently pinned to prevent auto-eviction
  - `is_busy` - Boolean indicating if the model has active requests or maintenance in progress
  - `is_streaming` - Boolean indicating if the model is actively generating output tokens (true after first chunk arrives, false when all streaming requests complete)
  - `backend_url` - URL of the backend server process handling this model (useful for debugging)
  - `pid` - The Process ID (PID) of the backend engine handling this model
  - `launch_command` - *(optional)* The command used to start the backend engine, as an array with the program first and its arguments after it. Every local backend has one. Cloud models don't, because they don't start a program. The values shown are the ones actually used, so a `ctx_size` of `auto` appears here as a real number, and any flags Lemonade added on its own are included.
  - `recipe` - Backend/device recipe used to load the model (e.g., `"ryzenai-llm"`, `"llamacpp"`, `"flm"`)
  - `recipe_options` - Options used to load the model (e.g., `"ctx_size"`, `"llamacpp_backend"`, `"llamacpp_args"`, `"whispercpp_args"`)
- `pinned_models` - Counts of pinned models currently loaded in memory per model type (e.g., `llm`, `embedding`, etc.)
- `max_models` - Maximum number of models that can be loaded simultaneously per type (set via `max_loaded_models` in [Server Configuration](../guide/configuration/README.md)):
  - `llm` - Maximum LLM/chat models
  - `embedding` - Maximum embedding models
  - `reranking` - Maximum reranking models
  - `transcription` - Maximum speech-to-text models
  - `image` - Maximum image models
  - `tts` - Maximum text-to-speech models
- `websocket_port` - *(optional)* Port of the WebSocket server for the [Realtime Audio Transcription API](./openai.md#ws-realtime) and [Log Streaming API](#log-streaming-api-websocket). Only present when the WebSocket server is running. The port is OS-assigned or set via `--websocket-port`.
- `telemetry` - Structured telemetry state object:
  - `enabled` - Boolean indicating if telemetry collection is active
  - `captures` - *(optional)* Array of captured telemetry components (e.g., `["inputs", "outputs", "thinking"]`), only present when `enabled` is `true`.

## `GET /v1/stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Performance statistics from the last request.

### Parameters

This endpoint does not take any parameters.

### Example request

```bash
curl http://localhost:13305/v1/stats
```

### Response format

```json
{
  "time_to_first_token": 2.14,
  "tokens_per_second": 33.33,
  "input_tokens": 128,
  "output_tokens": 5,
  "prompt_tokens": 9,
  "cache_tokens": 96,
  "request_count_total": 12,
  "input_tokens_total": 1536,
  "output_tokens_total": 60,
  "prompt_tokens_total": 108,
  "cache_tokens_total": 1152,
  "routing_decisions_total": 4,
  "routing_switches_total": 1
}
```

**Field Descriptions:**

- `time_to_first_token` - Time in seconds until the first token was generated
- `tokens_per_second` - Generation speed in tokens per second
- `input_tokens` - Number of tokens processed
- `output_tokens` - Number of tokens generated
- `prompt_tokens` - Total prompt tokens including cached tokens
- `cache_tokens` - Prompt tokens served from the backend's prefix cache on the last request (llama.cpp `timings.cache_n`, or `usage.prompt_tokens_details.cached_tokens` / Responses-API `input_tokens_details.cached_tokens` from OpenAI-compatible cloud providers). `null` when the last request did not report cache usage
- `*_total` - Cumulative counters since server start
- `routing_decisions_total` - Routing decisions made by `collection.router` dispatch
- `routing_switches_total` - Routing decisions that changed a conversation's routed model (a proxy for route ping-pong; conversations are identified by a hash of the system prompt and first user message)

## `GET /v1/system-stats`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Current host resource usage as measured by the Lemonade Server process. This endpoint is useful for first-party clients and dashboards that need lightweight runtime telemetry without scraping Prometheus.

### Parameters

This endpoint does not take any parameters.

### Example request

```bash
curl http://localhost:13305/v1/system-stats
```

### Response format

```json
{
  "cpu_percent": 12.3,
  "memory_gb": 8.4,
  "gpu_percent": 45.0,
  "vram_gb": 2.1,
  "npu_percent": null
}
```

**Field Descriptions:**

- `cpu_percent` - System CPU utilization percentage, or `null` when unavailable
- `memory_gb` - System RAM currently in use, in GiB
- `gpu_percent` - GPU utilization percentage, or `null` when unavailable
- `vram_gb` - GPU memory currently in use, in GiB, or `null` when unavailable
- `npu_percent` - NPU utilization percentage, or `null` when unavailable

GPU, VRAM, and NPU telemetry availability depends on the operating system and installed drivers. Unsupported values are returned as `null`.

## `GET /metrics`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Prometheus scrape endpoint for Lemonade Server. The endpoint returns Prometheus text exposition format and is intended to be scraped by Prometheus, not by Grafana directly.

Unlike most Lemonade API endpoints, `/metrics` is root-level only. It is not mounted under `/api/v0/`, `/api/v1/`, `/v0/`, or `/v1/`.

`HEAD /metrics` is also supported and returns `200 OK` with an empty body.

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

```bash
curl http://localhost:13305/metrics
```

With API-key auth:

```bash
curl http://localhost:13305/metrics \
  -H "Authorization: Bearer $LEMONADE_API_KEY"
```

### Response format

The response uses Prometheus text exposition format:

```text
# HELP lemonade_server_up Whether the Lemonade server is running.
# TYPE lemonade_server_up gauge
lemonade_server_up 1
# HELP lemonade_server_info Lemonade server build information.
# TYPE lemonade_server_info gauge
lemonade_server_info{version="10.4.0"} 1
```

Content type:

```text
text/plain; version=0.0.4; charset=utf-8
```

### Lemonade Metric Families

The authoritative metric-family list is generated by the `/metrics` implementation in [`src/cpp/server/server.cpp`](../../src/cpp/server/server.cpp). Search for `handle_metrics` and `metrics.describe(...)` to see the current names, types, labels, and descriptions.

Unsupported, unavailable, null, NaN, and infinity values are omitted rather than emitted as samples.

### llama.cpp Backend Metrics

When a loaded model uses the `llamacpp` recipe, Lemonade makes a best-effort scrape of the loaded backend process's private `/metrics` endpoint. Backend scrape failures do not fail the Lemonade `/metrics` response.

Scraped llama.cpp metrics are normalized under the `lemonade_llamacpp_*` prefix and labeled with the same Lemonade model metadata used by `lemonade_model_info`.

Lemonade starts llama.cpp backends with metrics enabled so these backend metrics are available whenever the backend supports them.

## `GET /v1/system-info`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

System information endpoint that provides complete hardware details and device enumeration.

### Example request

```bash
curl "http://localhost:13305/v1/system-info"
```

### Response format

```json
{
  "OS Version": "Windows-10-10.0.26100-SP0",
  "Processor": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
  "Physical Memory": "32.0 GB",
  "OEM System": "ASUS Zenbook S 16",
  "BIOS Version": "1.0.0",
  "CPU Max Clock": "5100 MHz",
  "Windows Power Setting": "Balanced",
  "model_storage": {
    "path": "/path/to/models",
    "used_bytes": 123456789,
    "total_bytes": 987654321,
    "free_bytes": 864197532
  },
  "devices": {
    "cpu": {
      "name": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
      "cores": 12,
      "threads": 24,
      "available": true,
      "family": "x86_64"
    },
    "amd_gpu": [
      {
        "name": "AMD Radeon(TM) 890M Graphics",
        "vram_gb": 0.5,
        "available": true,
        "family": "gfx1150"
      }
    ],
    "amd_npu": {
      "name": "AMD Ryzen AI 9 HX 375 w/ Radeon 890M",
      "power_mode": "Default",
      "available": true,
      "family": "XDNA2"
    }
  },
  "recipes": {
    "llamacpp": {
      "default_backend": "vulkan",
      "backends": {
        "vulkan": {
          "devices": ["cpu", "amd_gpu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "b7869"
        },
        "rocm": {
          "devices": ["amd_gpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install llamacpp:rocm"
        },
        "metal": {
          "devices": [],
          "state": "unsupported",
          "message": "Requires macOS",
          "action": ""
        },
        "cpu": {
          "devices": ["cpu"],
          "state": "update_required",
          "message": "Backend update is required before use.",
          "action": "lemonade backends install llamacpp:cpu"
        }
      }
    },
    "whispercpp": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["cpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install whispercpp:default"
        }
      }
    },
    "sd-cpp": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["cpu"],
          "state": "installable",
          "message": "Backend is supported but not installed.",
          "action": "lemonade backends install sd-cpp:default"
        }
      }
    },
    "flm": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["amd_npu"],
          "state": "installed",
          "message": "",
          "action": "",
          "version": "1.2.0"
        }
      }
    },
    "ryzenai-llm": {
      "default_backend": "default",
      "backends": {
        "default": {
          "devices": ["amd_npu"],
          "state": "installed",
          "message": "",
          "action": ""
        }
      }
    }
  }
}
```

**Field Descriptions:**

- **System fields:**
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

- `devices` - Hardware devices detected on the system (no software/support information)
  - `cpu` - CPU information (name, cores, threads)
  - `amd_gpu` - Array of AMD GPUs, both integrated and discrete (if present)
  - `nvidia_gpu` - Array of NVIDIA GPUs (if present)
  - `amd_npu` - AMD NPU device (if present)

- `recipes` - Software recipes and their backend support status
  - Each recipe (e.g., `llamacpp`, `whispercpp`, `flm`) contains:
    - `default_backend` - Preferred backend selected by server policy for this system (present when at least one backend is not `unsupported`)
    - `backends` - Available backends for this recipe
      - Each backend contains:
        - `devices` - List of devices **on this system** that support this backend (empty if not supported)
        - `state` - Backend lifecycle state: `unsupported`, `installable`, `update_required`, or `installed`
        - `message` - Human-readable status text for GUI and CLI users. Required for `unsupported`, `installable`, and `update_required`; empty for `installed`.
        - `action` - Actionable user instruction string. For install/update cases this is typically an exact CLI command; for other states it may be empty or another actionable value (for example, a URL).
        - `version` - Installed or configured backend version (when available)
- `cloud` - Cloud OpenAI-compatible providers configured on this server (omitted when no providers are installed). Contains:
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

## `POST /v1/install`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Install or update a backend for a specific recipe/backend pair, **or** register a cloud OpenAI-compatible provider. The request body is dispatched by the `backend` field: any value other than `"cloud"` is treated as a local backend install.

### Install a local backend

If the backend is already installed but outdated, this endpoint updates it to the configured version.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `recipe` | Yes | Recipe name (for example, `llamacpp`, `flm`, `whispercpp`, `sd-cpp`, `ryzenai-llm`) |
| `backend` | Yes | Backend name within the recipe (for example, `vulkan`, `rocm`, `cpu`, `default`) |
| `stream` | No | If `true`, returns Server-Sent Events with progress. Defaults to `false`. |
| `force` | No | If `true`, bypasses hardware filtering for `unsupported` backends and attempts installation anyway. Defaults to `false`. |

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

| Parameter | Required | Description |
|-----------|----------|-------------|
| `backend` | Yes | Must be the literal string `"cloud"`. |
| `provider` | Yes | Short identifier (e.g. `fireworks`). Used as the model-name prefix. |
| `base_url` | Yes | OpenAI-compatible base URL ending in `/v1` (or equivalent). |
| `api_key` | No | Optional. If set, stored in process memory; honors env-wins precedence (see `/v1/cloud/auth`). |
| `allow_insecure_http` | No | Default `false`. Must be `true` to send an API key to an `http://` base URL. |
| `auth_header_name` | No | Header carrying the API key. Must be a valid HTTP header name. Default `"Authorization"`. |
| `auth_header_prefix` | No | Value prefix before the key. Default `"Bearer "`; pass `""` for gateways that expect the bare key. |
| `wire_format` | No | `"openai"` (default) or `"anthropic"`. An `"anthropic"` provider is served from `POST /v1/messages` only; any other value returns 400. |

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

The endpoint is available at:

- `/v1/install/dry-run`
- `/api/v1/install/dry-run`
- `/v0/install/dry-run`
- `/api/v0/install/dry-run`

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `recipe` | Yes | Recipe name, for example `llamacpp`, `whispercpp`, or `vllm`. |
| `backend` | Yes | Backend name within the recipe, for example `vulkan`, `rocm`, or `rocm-nightly`. |
| `arch` | No | ROCm GPU architecture to use for this call, for example `gfx1201`. When provided, it overrides ROCm architecture detection while install parameters are resolved. When omitted, normal host detection is used; if resolution succeeds, `arch` is returned as `""` and `supported` is `true`. |

### Example request

```bash
curl -X POST http://localhost:13305/v1/install/dry-run \
  -H "Content-Type: application/json" \
  -d '{
    "recipe": "whispercpp",
    "backend": "rocm",
    "arch": "gfx1201"
  }'
```

### Response format

```json
{
  "recipe": "whispercpp",
  "backend": "rocm",
  "arch": "gfx1201",
  "repo": "lemonade-sdk/whisper.cpp-rocm",
  "version": "v1.8.4",
  "filename": "whisper-v1.8.4-linux-rocm-gfx120X.tar.gz",
  "url": "https://github.com/lemonade-sdk/whisper.cpp-rocm/releases/download/v1.8.4/whisper-v1.8.4-linux-rocm-gfx120X.tar.gz",
  "supports_split_archive": false,
  "supported": true
}
```

| Field | Description |
|-------|-------------|
| `recipe`, `backend`, `arch` | Echo the requested values. If `arch` was omitted, it is returned as an empty string. |
| `repo`, `version`, `filename` | Install parameters produced by the backend-specific resolver. The default version pin comes from `backend_versions.json`; runtime version policy can override it. |
| `url` | GitHub release-download URL constructed from `repo`, `version`, and `filename`. The endpoint does not check this URL. |
| `supported` | Whether Lemonade's local recipe/backend support matrix accepts the requested `arch`. This is not a release-asset existence check. When `arch` is omitted, it is `true` on a successful response. |
| `supports_split_archive` | Whether the recipe supports assets published as multiple archive parts. When `true`, the real download path can consult a `.partcount` manifest. |

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

## `POST /v1/uninstall`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Uninstall a backend for a specific recipe/backend pair, **or** remove a cloud provider. Dispatched by the `backend` field, mirroring `/v1/install`.

### Uninstall a local backend

If loaded models are using that backend, they are unloaded first.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `recipe` | Yes | Recipe name |
| `backend` | Yes | Backend name |

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

| Parameter | Required | Description |
|-----------|----------|-------------|
| `backend` | Yes | Must be the literal string `"cloud"`. |
| `provider` | Yes | Installed provider name. |

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

## `POST /v1/cloud/auth`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Set an in-memory API key for a previously-installed cloud provider, and trigger a refresh of that provider's discovered model list. The key lives in `lemond` process memory only — it is never written to disk and is cleared on `lemond` restart. For persistence across restarts, set `LEMONADE_<PROVIDER>_API_KEY` in `lemond`'s environment instead.

### Authentication precedence

If `LEMONADE_<PROVIDER>_API_KEY` is set in `lemond`'s environment, the env var takes precedence and this endpoint returns **409 Conflict** without storing the supplied key. This is the safety guarantee that lets an operator provision a "house" key via env without worrying about a client silently overriding it.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `provider` | Yes | Installed provider name. |
| `api_key` | Yes | API key to store in `lemond` process memory. |

### Example request

```bash
curl -X POST http://localhost:13305/v1/cloud/auth \
  -H "Content-Type: application/json" \
  -d '{
    "provider": "fireworks",
    "api_key": "fw-XXXXX"
  }'
```

### Response format (success — 200)

```json
{
  "provider": "fireworks",
  "auth_state": {
    "env_var_set": false,
    "runtime_key_set": true
  },
  "models_discovered": 12
}
```

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
| `404` | Provider is not installed. Call `POST /v1/install` with `backend:"cloud"` first. |

## `DELETE /v1/cloud/auth/{provider}`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Clear the in-memory API key for a provider. Any env-var-based key (`LEMONADE_<PROVIDER>_API_KEY`) remains in effect. If no env-var key is set, the provider's discovered models are evicted from the catalog since they are no longer authenticatable.

### Example request

```bash
curl -X DELETE http://localhost:13305/v1/cloud/auth/fireworks
```

### Response format

```json
{
  "provider": "fireworks",
  "cleared_runtime_key": true,
  "auth_state": {
    "env_var_set": false,
    "runtime_key_set": false
  }
}
```

`cleared_runtime_key` is `false` when no in-memory key was present (e.g., the only key was from the env var).

## Log Streaming API (WebSocket)
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Stream server logs over WebSocket. Clients connect, send a subscribe message, and receive a snapshot of recent log history followed by live log entries as they occur.

### Connection

The WebSocket server shares the same port as the [Realtime Audio Transcription API](./openai.md#ws-realtime). Discover the port via the [`/v1/health`](#get-v1health) endpoint (`websocket_port` field), then connect:

```
ws://localhost:<websocket_port>/logs/stream
```

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

## `GET /live`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lightweight liveness probe for load balancers and orchestrators. Unlike [`/v1/health`](#get-v1health), this endpoint does no work beyond confirming the process is up — it does not inspect loaded models or backends — so it is safe to poll at high frequency. `HEAD /live` is also supported and returns `200 OK` with an empty body.

Unlike the other endpoints on this page, `/live` is not versioned and is not mounted under the `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/` prefixes.

### Example request

```bash
curl http://localhost:13305/live
```

### Response format

```json
{"status":"ok"}
```

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

## Internal Endpoints

Internal endpoints are used for server control and configuration. By default, they are secured by `LEMONADE_ADMIN_API_KEY` (if set) to separate control privileges from standard inference operations.

## `POST /internal/telemetry/flush`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Forces the in-memory telemetry queue to flush all buffered trace spans immediately to the configured OTLP collector. This call blocks until all currently queued spans are serialized and sent.

#### Parameters

None.

Example request:

```bash
curl -X POST http://localhost:13305/internal/telemetry/flush
```

#### Response Format

Returns a JSON object indicating successful completion of the flush operation:

```json
{
  "status": "flushed"
}
```

## `GET /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Retrieves a list of all active model alias mappings.

#### Parameters

None.

Example request:

```bash
curl http://localhost:13305/internal/aliases
```

#### Response Format

Returns a JSON object containing an array of active alias objects:

```json
{
  "aliases": [
    {
      "alias": "my-alias-1",
      "target": "user.custom-llama",
      "downloaded": true,
      "recipe": "llamacpp"
    }
  ]
}
```

## `POST /internal/aliases`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Binds a model alias to a target model name.

#### Parameters

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `alias` | string | yes | The alias name to create or update. |
| `target` | string | yes | The target model name or canonical ID (also accepted as `model`). |

Example request:

```bash
curl -X POST http://localhost:13305/internal/aliases \
  -H "Content-Type: application/json" \
  -d '{
    "alias": "my-alias-1",
    "target": "user.custom-llama"
  }'
```

#### Response Format

Returns a JSON object confirming the alias binding:

```json
{
  "status": "ok",
  "alias": "my-alias-1",
  "target": "user.custom-llama"
}
```

Returns HTTP `400 Bad Request` if required fields are missing or invalid.

## `DELETE /internal/aliases/{alias}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Removes an existing model alias binding by name.

#### Parameters

| Path Parameter | Type | Description |
|----------------|------|-------------|
| `alias` | string | The alias name to remove. |

Example request:

```bash
curl -X DELETE http://localhost:13305/internal/aliases/my-alias-1
```

#### Response Format

Returns a JSON object confirming deletion:

```json
{
  "status": "deleted",
  "alias": "my-alias-1"
}
```

Returns HTTP `404 Not Found` if the alias does not exist.
