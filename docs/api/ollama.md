# Ollama-Compatible API

Lemonade supports the [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), allowing applications built for Ollama to work with Lemonade without modification.

To enable auto-detection by Ollama-integrated apps, configure the server to use the Ollama default port `11434`. See [Server Configuration](../guide/configuration/README.md#settings-reference) for how to change the port.

A model name may carry a `:latest` tag, which Lemonade ignores. Errors answer with a JSON body `{"error": "<message>"}`. An unknown model answers `404`, and so does a route that loads the model on first use when that load fails for any reason, except a load blocked by a `collection.router` residency constraint, which answers `409`. A backend error answers `500`.

<!-- BEGIN GENERATED: ollama.summary -->
| Method | Endpoint | Status | Description |
|--------|----------|--------|-------------|
| `POST` | [`/api/chat`](#post-apichat) | Supported | Generate the next chat message |
| `POST` | [`/api/generate`](#post-apigenerate) | Supported | Continue a prompt or generate an image |
| `GET` | [`/api/tags`](#get-apitags) | Supported | List downloaded models |
| `POST` | [`/api/show`](#post-apishow) | Supported | Describe a model |
| `DELETE` | [`/api/delete`](#delete-apidelete) | Supported | Delete a model |
| `POST` | [`/api/pull`](#post-apipull) | Supported | Download a model |
| `POST` | [`/api/embed`](#post-apiembed) | Supported | Embed one or more texts |
| `POST` | [`/api/embeddings`](#post-apiembeddings) | Supported | Embed one text in the older format |
| `GET` | [`/api/ps`](#get-apips) | Supported | List loaded models |
| `GET` | [`/api/version`](#get-apiversion) | Supported | Report the Ollama version |
| `POST` | [`/api/create`](#post-apicreate) | Not supported | Create a model from a Modelfile |
| `POST` | [`/api/copy`](#post-apicopy) | Not supported | Copy a model |
| `POST` | [`/api/push`](#post-apipush) | Not supported | Push a model to a registry |
| `POST` | [`/api/blobs/{digest}`](#post-apiblobsdigest) | Not supported | Upload a blob |
<!-- END GENERATED: ollama.summary -->

<!-- BEGIN GENERATED: ollama.chat -->
## `POST /api/chat`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates the next assistant message for a conversation, loading the model on first use. Lemonade runs it as an OpenAI chat completion and converts the result.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `messages` | No | Conversation so far. Each message has a `role` and `content`, plus optional `images` (base64 strings), `tool_calls`, and, on `tool` messages, `tool_name` or `tool_call_id`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` streams the response as newline-delimited JSON; `false` sends one response once complete. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | Sampling options: `temperature`, `top_p`, `seed`, `stop`, `num_predict` (maximum generated tokens) and `repeat_penalty`, which is sent as OpenAI's `frequency_penalty`. These keys are also accepted at the top level, where they win. `num_ctx` sets the context size when this request loads the model, unless `ctx_size` is sent. Other keys are ignored. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `tools` | No | A list of tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` | No | `json` constrains the reply to a JSON object. A JSON schema is accepted but not applied. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `think` | No | `true`, `false`, `low`, `medium`, `high` or `max`. Not applied: reasoning models think regardless. Qwen3 models skip their reasoning when the prompt ends with `/no_think`. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `keep_alive` | No | `0` with an empty input unloads the model, answering with `done_reason: "unload"`. Other values are ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A request with tools and streaming runs non-streaming on the backend and answers with two NDJSON lines: the whole message, then the final line.

### Examples

#### Non-streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/chat" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/chat \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": false
        }'
    ```

**Response:** `200 OK`

```json
{
  "model": "Qwen3-0.6B-GGUF",
  "created_at": "2024-01-01T00:00:00Z",
  "message": {"role": "assistant", "content": "The capital of France is **Paris**."},
  "done": true,
  "done_reason": "stop",
  "prompt_eval_count": 19,
  "eval_count": 14,
  "total_duration": 0,
  "load_duration": 0,
  "prompt_eval_duration": 10670000,
  "eval_duration": 40000000
}
```

#### Streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/chat" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": true
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/chat \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": true
        }'
    ```

**Response:** `200 OK`

```text
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": ""}, "done": false}
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": "The"}, "done": false}
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": " capital"}, "done": false}
...
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": ""}, "done": true, "done_reason": "stop"}
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": ""}, "done": true, "done_reason": "stop", "prompt_eval_count": 0, "eval_count": 0, "total_duration": 0, "load_duration": 0, "prompt_eval_duration": 0, "eval_duration": 0}
```

### Response format

#### JSON

- `model` - The model that generated the response.
- `created_at` - Always `2024-01-01T00:00:00Z`.
- `message` - The assistant's message; in a streamed line, the next piece of it.
  - `role` - Always `assistant`.
  - `content` - The message text.
  - `thinking` - Reasoning models only: the model's thinking.
  - `tool_calls` - Calls to the request's tools, with arguments as objects.
- `done` - `false` while the message streams; `true` once it ends, and on a response that is not streamed.
- `done_reason` - `stop`, `length`, `tool_calls`, or `unload` after an unload request.
- `prompt_eval_count` - Prompt tokens.
- `eval_count` - Generated tokens.
- `total_duration`, `load_duration` - Always `0`.
- `prompt_eval_duration` - Prompt processing time in nanoseconds, when the backend reports timings; otherwise `0`.
- `eval_duration` - Generation time in nanoseconds, when the backend reports timings; otherwise `0`.

#### NDJSON

Each line carries the next message piece with `done: false`. When the message ends, a line with `done: true` and the real `done_reason` follows, then a last `done: true` line with the token counts, which are `0` when the backend reports no usage, and `done_reason` `stop`.

- `model` - The model that generated the response.
- `created_at` - Always `2024-01-01T00:00:00Z`.
- `message` - The assistant's message; in a streamed line, the next piece of it.
  - `role` - Always `assistant`.
  - `content` - The message text.
  - `thinking` - Reasoning models only: the model's thinking.
  - `tool_calls` - Calls to the request's tools, with arguments as objects.
- `done` - `false` while the message streams; `true` once it ends, and on a response that is not streamed.
- `done_reason` - `stop`, `length`, `tool_calls`, or `unload` after an unload request.
- `prompt_eval_count` - Prompt tokens.
- `eval_count` - Generated tokens.
- `total_duration`, `load_duration` - Always `0`.
- `prompt_eval_duration` - Prompt processing time in nanoseconds on a request with tools, when the backend reports timings; otherwise `0`.
- `eval_duration` - Generation time in nanoseconds on a request with tools, when the backend reports timings; otherwise `0`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `model` fails to load for any reason other than a residency conflict, such as naming no model or a failed download |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `500` |  | The backend reports an error, whatever its own status |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: ollama.chat -->

<!-- BEGIN GENERATED: ollama.generate -->
## `POST /api/generate`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Continues a prompt, loading the model on first use. Lemonade runs it as an OpenAI text completion and converts the result. Naming an image model generates an image instead.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Text to continue, or the image to generate for an image model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` streams the response as newline-delimited JSON; `false` sends one response once complete. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | Sampling options: `temperature`, `top_p`, `stop`, `num_predict` (maximum generated tokens), `repeat_penalty`, which is sent as OpenAI's `frequency_penalty`, and the `seed`, `width`, `height`, `steps` and `cfg_scale` documented below. A key given at the top level wins. `num_ctx` sets the context size when this request loads the model, unless `ctx_size` is sent. Other keys are ignored. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `width` | No | Image models only: image width in pixels. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | Image models only: image height in pixels. Defaults to `512`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Image models only: inference steps. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Image models only: classifier-free guidance scale. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible output. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `keep_alive` | No | `0` with an empty input unloads the model, answering with `done_reason: "unload"`. Other values are ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `system` | No | System prompt. Ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `format` | No | Output format constraint. Ignored. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

Image generation never streams.

### Examples

#### Non-streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/generate" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "options": {"num_predict": 16},
          "stream": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/generate \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "options": {"num_predict": 16},
          "stream": false
        }'
    ```

**Response:** `200 OK`

```json
{
  "model": "Qwen3-0.6B-GGUF",
  "created_at": "2024-01-01T00:00:00Z",
  "response": " Paris, right? Yes, that's correct. But there are other countries with",
  "done": true,
  "done_reason": "stop",
  "context": [],
  "prompt_eval_count": 5,
  "eval_count": 16,
  "total_duration": 0,
  "load_duration": 0,
  "prompt_eval_duration": 0,
  "eval_duration": 0
}
```

#### Streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/generate" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "options": {"num_predict": 16},
          "stream": true
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/generate \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "options": {"num_predict": 16},
          "stream": true
        }'
    ```

**Response:** `200 OK`

```text
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": " Paris", "done": false}
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": ",", "done": false}
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": " which", "done": false}
...
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": "", "done": true, "done_reason": "length"}
{"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": "", "done": true, "done_reason": "stop", "context": [], "prompt_eval_count": 5, "eval_count": 16, "total_duration": 0, "load_duration": 0, "prompt_eval_duration": 0, "eval_duration": 0}
```

### Response format

#### JSON

- `model` - The model that generated the response.
- `created_at` - Always `2024-01-01T00:00:00Z`.
- `response` - Generated text. Empty for an image model.
- `image` - Image models only: the generated image, base64-encoded.
- `done` - `true` on a response that is not streamed, and on the last two lines of a stream.
- `done_reason` - `stop`, or `unload` after an unload request.
- `context` - Always empty.
- `prompt_eval_count` - Prompt tokens.
- `eval_count` - Generated tokens.
- `total_duration`, `load_duration`, `prompt_eval_duration`, `eval_duration` - Always `0`.

#### NDJSON

Each line carries the next piece of text with `done: false`. When the text ends, a line with `done: true` and the real `done_reason` follows, then a last `done: true` line with the token counts and `done_reason` `stop`.

- `model` - The model that generated the response.
- `created_at` - Always `2024-01-01T00:00:00Z`.
- `response` - Generated text. Empty for an image model.
- `image` - Image models only: the generated image, base64-encoded.
- `done` - `true` on a response that is not streamed, and on the last two lines of a stream.
- `done_reason` - `stop`, `length`, or `unload` after an unload request.
- `context` - Always empty.
- `prompt_eval_count` - Prompt tokens.
- `eval_count` - Generated tokens.
- `total_duration`, `load_duration`, `prompt_eval_duration`, `eval_duration` - Always `0`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `model` fails to load for any reason other than a residency conflict, such as naming no model or a failed download |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `500` |  | The backend reports an error, whatever its own status |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: ollama.generate -->

<!-- BEGIN GENERATED: ollama.tags -->
## `GET /api/tags`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the downloaded models in Ollama's format.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/tags"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/tags
    ```

**Response:** `200 OK`

```json
{
  "models": [
    {
      "name": "Phishing-Email-Detection-ONNX:latest",
      "model": "Phishing-Email-Detection-ONNX:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 268435456,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "onnxruntime",
        "families": ["onnxruntime"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    ... 10 more
  ]
}
```

### Response format

- `models` - The downloaded models.
  - `name` - Lemonade model name with a `:latest` tag.
  - `model` - Same as `name`.
  - `modified_at` - Always `2024-01-01T00:00:00Z`.
  - `size` - Size in bytes, from the registry's estimate.
  - `digest` - Always an all-zero sha256.
  - `details` - Inferred from the model name, recipe and checkpoint.
    - `parent_model` - Always empty.
    - `format` - Always `gguf`.
    - `family` - The model's recipe, e.g. `llamacpp`.
    - `families` - A list holding only `family`.
    - `parameter_size` - From the model name, e.g. `0.6B`; empty when the name has none.
    - `quantization_level` - From the checkpoint, e.g. `Q4_0`; empty when the checkpoint has none.

### Errors

| Status | When |
|--------|------|
| `500` | The request fails unexpectedly |
<!-- END GENERATED: ollama.tags -->

<!-- BEGIN GENERATED: ollama.show -->
## `POST /api/show`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Describes one registered model in Ollama's format.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` or `name` | Yes | The model's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/show" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "Qwen3-0.6B-GGUF"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/show \
      -H "Content-Type: application/json" \
      -d '{"model": "Qwen3-0.6B-GGUF"}'
    ```

**Response:** `200 OK`

```json
{
  "modelfile": "# Modelfile generated by Lemonade\nFROM unsloth/Qwen3-0.6B-GGUF:Q4_0",
  "parameters": "num_ctx -1",
  "template": "",
  "details": {
    "parent_model": "",
    "format": "gguf",
    "family": "llamacpp",
    "families": ["llamacpp"],
    "parameter_size": "0.6B",
    "quantization_level": "Q4_0"
  },
  "capabilities": ["completion", "tools", "thinking"],
  "model_info": {
    "general.architecture": "llamacpp",
    "general.file_type": 0,
    "general.parameter_count": 0,
    "general.quantization_version": 0,
    "llamacpp.context_length": -1
  }
}
```

### Response format

- `modelfile` - A `FROM` line naming the checkpoint.
- `parameters` - `num_ctx` followed by the model's configured context size, which is `-1` when Lemonade sizes the context at load time.
- `template` - Always empty.
- `details` - Inferred from the model name, recipe and checkpoint.
  - `parent_model` - Always empty.
  - `format` - Always `gguf`.
  - `family` - The model's recipe, e.g. `llamacpp`.
  - `families` - A list holding only `family`.
  - `parameter_size` - From the model name, e.g. `0.6B`; empty when the name has none.
  - `quantization_level` - From the checkpoint, e.g. `Q4_0`; empty when the checkpoint has none.
- `capabilities` - From the model's labels: `completion` for chat models, `embedding` for embedding models, plus `tools`, `vision` and `thinking` for the `tool-calling` (or `tools`), `vision` and `reasoning` labels.
- `model_info` - `general.architecture` (the recipe) and `<recipe>.context_length` (the context size in `parameters`); the other keys are always `0`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `model` names no model in the registry, or one filtered out on this system |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: ollama.show -->

<!-- BEGIN GENERATED: ollama.delete -->
## `DELETE /api/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Deletes a downloaded model's files, unloading it first when it is loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` or `name` | Yes | The model's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Success answers `200` with an empty body, as Ollama does.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/delete" `
      -Method DELETE `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "RealESRGAN-x4plus-anime"}'
    ```

=== "Bash"

    ```bash
    curl -X DELETE http://localhost:13305/api/delete \
      -H "Content-Type: application/json" \
      -d '{"model": "RealESRGAN-x4plus-anime"}'
    ```

**Response:** `200 OK`

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `model` names no model in the registry, or one filtered out on this system |
| `500` |  | Deleting fails for another reason, for example for an `extra.` model, a file that stays locked after the retries, or a failed unload |
<!-- END GENERATED: ollama.delete -->

<!-- BEGIN GENERATED: ollama.pull -->
## `POST /api/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Downloads a registered model, or updates it when its registry has a newer version.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` or `name` | Yes | The model's name. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | `true` streams the response as newline-delimited JSON; `false` sends one response once complete. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Only models already in Lemonade's registry can be pulled; use [`POST /v1/pull`](./lemonade.md#post-v1pull) to register a new one.

### Examples

#### Streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "Qwen3-0.6B-GGUF", "stream": true}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/pull \
      -H "Content-Type: application/json" \
      -d '{"model": "Qwen3-0.6B-GGUF", "stream": true}'
    ```

**Response:** `200 OK`

```text
{"status": "pulling manifest"}
{"status": "downloading Qwen3-0.6B-Q4_0.gguf", "digest": "sha256:Qwen3-0.6B-Q4_0.gguf", "completed": 0, "total": 382156480}
{"status": "downloading Qwen3-0.6B-Q4_0.gguf", "digest": "sha256:Qwen3-0.6B-Q4_0.gguf", "completed": 382156480, "total": 382156480}
...
{"status": "success"}
{"status": "success"}
```

#### Non-streaming

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "RealESRGAN-x4plus-anime", "stream": false}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/pull \
      -H "Content-Type: application/json" \
      -d '{"model": "RealESRGAN-x4plus-anime", "stream": false}'
    ```

**Response:** `200 OK`

```json
{"status": "success"}
```

### Response format

#### NDJSON

The first line is `pulling manifest`, then one line per progress report, then two `success` lines.

- `status` - `pulling manifest`, `downloading <file>`, or `success`.
- `digest` - Download lines: `sha256:` followed by the file name.
- `completed` - Download lines: bytes of the file downloaded so far.
- `total` - Download lines: the file's size in bytes.
- `error` - Only on a failed download's last line.

#### JSON

- `status` - Always `success`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` | `lemond_offline` | `offline` is set to `true` |
| `404` |  | `model` names no model in the registry, or one filtered out on this system |
| `500` |  | A non-streaming download fails |
<!-- END GENERATED: ollama.pull -->

<!-- BEGIN GENERATED: ollama.embed -->
## `POST /api/embed`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Returns one embedding per input text, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to embed, or an array of texts. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | `num_ctx` sets the context size when this request loads the model, unless `ctx_size` is sent. Other keys are ignored. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/embed" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "nomic-embed-text-v1-GGUF", "input": "Why is the sky blue?"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/embed \
      -H "Content-Type: application/json" \
      -d '{"model": "nomic-embed-text-v1-GGUF", "input": "Why is the sky blue?"}'
    ```

**Response:** `200 OK`

```json
{
  "model": "nomic-embed-text-v1-GGUF",
  "embeddings": [
    [0.029926229268312454, 0.050356026738882065, -0.00871499814093113, -0.018384544178843498, 0.016114410012960434, 0.08822233229875565, -0.015040549449622631, -7.978447683854029e-05, ... 760 more]
  ],
  "total_duration": 0,
  "load_duration": 0,
  "prompt_eval_count": 0
}
```

### Response format

- `model` - The model that produced the embeddings.
- `embeddings` - One vector per input, in input order.
- `total_duration`, `load_duration`, `prompt_eval_count` - Always `0`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `model` fails to load for any reason other than a residency conflict, such as naming no model or a failed download |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `500` |  | The backend reports an error, whatever its own status |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: ollama.embed -->

<!-- BEGIN GENERATED: ollama.embeddings -->
## `POST /api/embeddings`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Returns the embedding of one text in Ollama's older format, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` or `input` | Yes | Text to embed. For an array, only the first text's embedding is returned. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | `num_ctx` sets the context size when this request loads the model, unless `ctx_size` is sent. Other keys are ignored. | <sub>![Status](https://img.shields.io/badge/partial-yellow)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/embeddings" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "nomic-embed-text-v1-GGUF", "prompt": "Why is the sky blue?"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/embeddings \
      -H "Content-Type: application/json" \
      -d '{"model": "nomic-embed-text-v1-GGUF", "prompt": "Why is the sky blue?"}'
    ```

**Response:** `200 OK`

```json
{
  "model": "nomic-embed-text-v1-GGUF",
  "embedding": [0.029926229268312454, 0.050356026738882065, -0.00871499814093113, -0.018384544178843498, 0.016114410012960434, 0.08822233229875565, -0.015040549449622631, -7.978447683854029e-05, ... 760 more]
}
```

### Response format

- `model` - The model that produced the embedding.
- `embedding` - The text's embedding vector.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `404` |  | `model` fails to load for any reason other than a residency conflict, such as naming no model or a failed download |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `500` |  | The backend reports an error, whatever its own status |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: ollama.embeddings -->

<!-- BEGIN GENERATED: ollama.ps -->
## `GET /api/ps`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the loaded models in Ollama's format.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

[`GET /v1/health`](./lemonade.md#get-v1health) reports the loaded models in full.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/ps"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/ps
    ```

**Response:** `200 OK`

```json
{
  "models": [
    {
      "name": "Qwen3-0.6B-GGUF:latest",
      "model": "Qwen3-0.6B-GGUF:latest",
      "size": 0,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "llamacpp",
        "families": ["llamacpp"],
        "parameter_size": "0.6B",
        "quantization_level": "Q4_0"
      },
      "expires_at": "2099-01-01T00:00:00Z",
      "size_vram": 0
    }
  ]
}
```

### Response format

- `models` - The loaded models.
  - `name` - Lemonade model name with a `:latest` tag.
  - `model` - Same as `name`.
  - `size` - Always `0`.
  - `digest` - Always an all-zero sha256.
  - `details` - Inferred from the model name, recipe and checkpoint.
    - `parent_model` - Always empty.
    - `format` - Always `gguf`.
    - `family` - The model's recipe, e.g. `llamacpp`.
    - `families` - A list holding only `family`.
    - `parameter_size` - From the model name, e.g. `0.6B`; empty when the name has none.
    - `quantization_level` - From the checkpoint, e.g. `Q4_0`; empty when the checkpoint has none.
  - `expires_at` - Always `2099-01-01T00:00:00Z`.
  - `size_vram` - Always `0`.

### Errors

| Status | When |
|--------|------|
| `500` | The request fails unexpectedly |
<!-- END GENERATED: ollama.ps -->

<!-- BEGIN GENERATED: ollama.version -->
## `GET /api/version`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports an Ollama version recent enough for Ollama clients' version checks. [`GET /v1/health`](./lemonade.md#get-v1health) reports Lemonade's own version.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/version"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/version
    ```

**Response:** `200 OK`

```json
{"version": "0.16.1"}
```

### Response format

- `version` - The Ollama version Lemonade reports to clients.
<!-- END GENERATED: ollama.version -->

<!-- BEGIN GENERATED: ollama.create -->
## `POST /api/create`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Register a model with [`POST /v1/pull`](./lemonade.md#post-v1pull) instead.

Requires `LEMONADE_API_KEY` when it is set.

### Errors

| Status | When |
|--------|------|
| `501` | Lemonade does not support this Ollama operation |
<!-- END GENERATED: ollama.create -->

<!-- BEGIN GENERATED: ollama.copy -->
## `POST /api/copy`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>



Requires `LEMONADE_API_KEY` when it is set.

### Errors

| Status | When |
|--------|------|
| `501` | Lemonade does not support this Ollama operation |
<!-- END GENERATED: ollama.copy -->

<!-- BEGIN GENERATED: ollama.push -->
## `POST /api/push`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>



Requires `LEMONADE_API_KEY` when it is set.

### Errors

| Status | When |
|--------|------|
| `501` | Lemonade does not support this Ollama operation |
<!-- END GENERATED: ollama.push -->

<!-- BEGIN GENERATED: ollama.blobs -->
## `POST /api/blobs/{digest}`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>



Requires `LEMONADE_API_KEY` when it is set.

### Errors

| Status | When |
|--------|------|
| `501` | Lemonade does not support this Ollama operation |
<!-- END GENERATED: ollama.blobs -->
