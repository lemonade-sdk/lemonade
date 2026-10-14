# Ollama-Compatible API

Lemonade supports the [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), allowing applications built for Ollama to work with Lemonade without modification.

To enable auto-detection by Ollama-integrated apps, configure the server to use the Ollama default port `11434`. See [Server Configuration](../guide/configuration/README.md#settings-reference) for how to change the port.

A model name may carry a `:latest` tag, which Lemonade ignores. Errors answer with a JSON body `{"error": "<message>"}`, and an unknown model answers `404`.

<!-- BEGIN GENERATED: ollama.summary -->
| Method | Endpoint | Status | Description |
|--------|----------|--------|-------------|
| `POST` | [`/api/chat`](#post-apichat) | Supported | Chat completion, streaming or not |
| `POST` | [`/api/generate`](#post-apigenerate) | Supported | Text completion or image generation |
| `GET` | [`/api/tags`](#get-apitags) | Supported | Lists downloaded models |
| `POST` | [`/api/show`](#post-apishow) | Supported | Model details |
| `DELETE` | [`/api/delete`](#delete-apidelete) | Supported | Delete a model |
| `POST` | [`/api/pull`](#post-apipull) | Supported | Download with progress |
| `POST` | [`/api/embed`](#post-apiembed) | Supported | New embeddings format |
| `POST` | [`/api/embeddings`](#post-apiembeddings) | Supported | Legacy embeddings |
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
| `model` | Yes | Model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `messages` | Yes | Conversation so far. Each message has a `role` and `content`, plus optional `images` (base64 strings), `tool_calls`, and, on `tool` messages, `tool_name` or `tool_call_id`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream newline-delimited JSON as tokens are generated. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | Sampling options: `temperature`, `top_p`, `seed`, `stop`, `num_predict` (maximum generated tokens) and `repeat_penalty`. These keys are also accepted at the top level, where they win. `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | Tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` | No | `json` constrains the reply to a JSON object. JSON schemas are not supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `think` | No | Not applied: reasoning models think regardless. Qwen3 models skip their reasoning when the prompt ends with `/no_think`. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `keep_alive` | No | `0` with an empty `messages` array unloads the model, answering with `done_reason: "unload"`. Other values are ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

A request with tools and streaming runs non-streaming on the backend and answers with two NDJSON lines: the whole message, then the final line.

### Example request

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

=== "PowerShell (streaming)"

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

=== "Bash (streaming)"

    ```bash
    curl http://localhost:13305/api/chat \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"role": "user", "content": "What is the capital of France? /no_think"}],
          "stream": true
        }'
    ```

### Response format

=== "Non-streaming responses"

    `200`

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

    **Field Descriptions:**

    - `created_at` - Always 2024-01-01T00:00:00Z.
    - `message`
      - `thinking` - Reasoning models only: the model's thinking.
      - `tool_calls` - Calls to the request's tools, with arguments as objects.
    - `done_reason` - stop, length, tool_calls, or unload after an unload request.
    - `prompt_eval_count` - Prompt tokens.
    - `eval_count` - Generated tokens.
    - `total_duration`, `load_duration` - Always 0.
    - `prompt_eval_duration` - Prompt processing time in nanoseconds, when the backend reports timings; otherwise 0.
    - `eval_duration` - Generation time in nanoseconds, when the backend reports timings; otherwise 0.

=== "Streaming responses"

    `200`

    ```text
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": ""}, "done": false}
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": "The"}, "done": false}
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": " capital"}, "done": false}
    ...
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": ""}, "done": true, "done_reason": "stop"}
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "message": {"role": "assistant", "content": ""}, "done": true, "done_reason": "stop", "prompt_eval_count": 0, "eval_count": 0, "total_duration": 0, "load_duration": 0, "prompt_eval_duration": 0, "eval_duration": 0}
    ```

    **Field Descriptions:**

    - `created_at` - Always 2024-01-01T00:00:00Z.
    - `message`
      - `thinking` - Reasoning models only: the model's thinking.
      - `tool_calls` - Calls to the request's tools, with arguments as objects.
    - `done_reason` - stop, length, tool_calls, or unload after an unload request.
    - `prompt_eval_count` - Prompt tokens.
    - `eval_count` - Generated tokens.
    - `total_duration`, `load_duration` - Always 0.
    - `prompt_eval_duration` - Prompt processing time in nanoseconds, when the backend reports timings; otherwise 0.
    - `eval_duration` - Generation time in nanoseconds, when the backend reports timings; otherwise 0.
<!-- END GENERATED: ollama.chat -->

<!-- BEGIN GENERATED: ollama.generate -->
## `POST /api/generate`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Continues a prompt, loading the model on first use. Lemonade runs it as an OpenAI text completion and converts the result. Naming an image model generates an image instead.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text to continue, or the image to generate for an image model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream newline-delimited JSON as tokens are generated. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | Sampling options: `temperature`, `top_p`, `stop`, `num_predict` (maximum generated tokens), `repeat_penalty`, and the `seed`, `width`, `height`, `steps` and `cfg_scale` documented below. A key given at the top level wins. `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `width` | No | Image models only: image width in pixels. Defaults to 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | Image models only: image height in pixels. Defaults to 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Image models only: inference steps. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Image models only: classifier-free guidance scale. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible output. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `keep_alive` | No | `0` with an empty `prompt` unloads the model, answering with `done_reason: "unload"`. Other values are ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `system` | No | System prompt. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `format` | No | Output format constraint. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

Image generation never streams.

### Example request

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

=== "PowerShell (streaming)"

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

=== "Bash (streaming)"

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

### Response format

=== "Non-streaming responses"

    `200`

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

    **Field Descriptions:**

    - `created_at` - Always 2024-01-01T00:00:00Z.
    - `response` - Generated text. Empty for an image model.
    - `image` - Image models only: the generated image, base64-encoded.
    - `done_reason` - stop, length, or unload after an unload request.
    - `context` - Always empty.
    - `prompt_eval_count` - Prompt tokens.
    - `eval_count` - Generated tokens.
    - `total_duration`, `load_duration`, `prompt_eval_duration`, `eval_duration` - Always 0.

=== "Streaming responses"

    `200`

    ```text
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": " Paris", "done": false}
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": ",", "done": false}
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": " which", "done": false}
    ...
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": "", "done": true, "done_reason": "length"}
    {"model": "Qwen3-0.6B-GGUF", "created_at": "2024-01-01T00:00:00Z", "response": "", "done": true, "done_reason": "stop", "context": [], "prompt_eval_count": 5, "eval_count": 16, "total_duration": 0, "load_duration": 0, "prompt_eval_duration": 0, "eval_duration": 0}
    ```

    **Field Descriptions:**

    - `created_at` - Always 2024-01-01T00:00:00Z.
    - `response` - Generated text. Empty for an image model.
    - `image` - Image models only: the generated image, base64-encoded.
    - `done_reason` - stop, length, or unload after an unload request.
    - `context` - Always empty.
    - `prompt_eval_count` - Prompt tokens.
    - `eval_count` - Generated tokens.
    - `total_duration`, `load_duration`, `prompt_eval_duration`, `eval_duration` - Always 0.
<!-- END GENERATED: ollama.generate -->

<!-- BEGIN GENERATED: ollama.tags -->
## `GET /api/tags`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the downloaded models in Ollama's format.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/tags"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/tags
    ```

### Response format

`200`

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
    {
      "name": "Qwen3-0.6B-GGUF:latest",
      "model": "Qwen3-0.6B-GGUF:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 382252089,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "llamacpp",
        "families": ["llamacpp"],
        "parameter_size": "0.6B",
        "quantization_level": "Q4_0"
      }
    },
    {
      "name": "Qwen3-0.6B-HRX:latest",
      "model": "Qwen3-0.6B-HRX:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 382252089,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "llamacpp-hrx",
        "families": ["llamacpp-hrx"],
        "parameter_size": "0.6B",
        "quantization_level": "Q4_0"
      }
    },
    {
      "name": "RealESRGAN-x4plus:latest",
      "model": "RealESRGAN-x4plus:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 66571993,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "sd-cpp",
        "families": ["sd-cpp"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    {
      "name": "SD-Turbo:latest",
      "model": "SD-Turbo:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 5218385264,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "sd-cpp",
        "families": ["sd-cpp"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    {
      "name": "TRELLIS-3D:latest",
      "model": "TRELLIS-3D:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 32963873996,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "trellis",
        "families": ["trellis"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    {
      "name": "ThinkSound-SFX:latest",
      "model": "ThinkSound-SFX:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 6871947673,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "thinksound",
        "families": ["thinksound"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    {
      "name": "Whisper-Tiny:latest",
      "model": "Whisper-Tiny:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 77309411,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "whispercpp",
        "families": ["whispercpp"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    {
      "name": "jina-reranker-v1-tiny-en-GGUF:latest",
      "model": "jina-reranker-v1-tiny-en-GGUF:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 36507222,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "llamacpp",
        "families": ["llamacpp"],
        "parameter_size": "",
        "quantization_level": "Q8_0"
      }
    },
    {
      "name": "kokoro-v1:latest",
      "model": "kokoro-v1:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 0,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "kokoro",
        "families": ["kokoro"],
        "parameter_size": "",
        "quantization_level": ""
      }
    },
    {
      "name": "nomic-embed-text-v1-GGUF:latest",
      "model": "nomic-embed-text-v1-GGUF:latest",
      "modified_at": "2024-01-01T00:00:00Z",
      "size": 78383153,
      "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "details": {
        "parent_model": "",
        "format": "gguf",
        "family": "llamacpp",
        "families": ["llamacpp"],
        "parameter_size": "",
        "quantization_level": "Q4_K_S"
      }
    }
  ]
}
```

**Field Descriptions:**

- `models`
  - `name` - Lemonade model name with a :latest tag.
  - `model` - Same as name.
  - `modified_at` - Always 2024-01-01T00:00:00Z.
  - `size` - Size in bytes, from the registry's estimate.
  - `digest` - Always an all-zero sha256.
  - `details` - Inferred from the model name, recipe and checkpoint.
    - `parent_model` - Always empty.
    - `format` - Always gguf.
    - `family` - The model's recipe, e.g. llamacpp.
    - `parameter_size` - From the model name, e.g. 0.6B; empty when the name has none.
    - `quantization_level` - From the checkpoint, e.g. Q4_0; empty when the checkpoint has none.
<!-- END GENERATED: ollama.tags -->

<!-- BEGIN GENERATED: ollama.show -->
## `POST /api/show`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Describes one registered model in Ollama's format.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to describe. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `name` | No | Older name for `model`; wins when both are given. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

`capabilities` follows the model's labels: `completion` for chat models, `embedding` for embedding models, plus `tools`, `vision` and `thinking` when the model carries the matching label.

### Example request

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

### Response format

`200`

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

**Field Descriptions:**

- `modelfile` - A FROM line naming the checkpoint.
- `parameters` - `num_ctx` followed by the model's configured context size.
- `template` - Always empty.
- `details` - Inferred from the model name, recipe and checkpoint.
  - `parent_model` - Always empty.
  - `format` - Always gguf.
  - `family` - The model's recipe, e.g. llamacpp.
  - `parameter_size` - From the model name, e.g. 0.6B; empty when the name has none.
  - `quantization_level` - From the checkpoint, e.g. Q4_0; empty when the checkpoint has none.
- `model_info` - `general.architecture` (the recipe) and `<recipe>.context_length`; the other keys are always 0.
<!-- END GENERATED: ollama.show -->

<!-- BEGIN GENERATED: ollama.delete -->
## `DELETE /api/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Deletes a downloaded model's files, unloading it first when it is loaded.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to delete. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `name` | No | Older name for `model`; wins when both are given. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Success answers `200` with an empty body, as Ollama does. A model this system does not support also answers `404`.

### Example request

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

### Response format

`200`
<!-- END GENERATED: ollama.delete -->

<!-- BEGIN GENERATED: ollama.pull -->
## `POST /api/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Downloads a registered model, or updates it when its registry has a newer version.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to download. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `name` | No | Older name for `model`; wins when both are given. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream newline-delimited JSON progress. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Only models already in Lemonade's registry can be pulled; use [`POST /v1/pull`](./lemonade.md#post-v1pull) to register a new one.

A failed download ends the stream with an `error` line.

In offline mode the request answers `400` with code `lemond_offline`.

### Example request

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

=== "PowerShell (non-streaming)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/pull" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"model": "RealESRGAN-x4plus-anime", "stream": false}'
    ```

=== "Bash (non-streaming)"

    ```bash
    curl http://localhost:13305/api/pull \
      -H "Content-Type: application/json" \
      -d '{"model": "RealESRGAN-x4plus-anime", "stream": false}'
    ```

### Response format

=== "Streaming responses"

    `200`

    ```text
    {"status": "pulling manifest"}
    {"status": "downloading Qwen3-0.6B-Q4_0.gguf", "digest": "sha256:Qwen3-0.6B-Q4_0.gguf", "completed": 0, "total": 382156480}
    {"status": "downloading Qwen3-0.6B-Q4_0.gguf", "digest": "sha256:Qwen3-0.6B-Q4_0.gguf", "completed": 382156480, "total": 382156480}
    ...
    {"status": "success"}
    {"status": "success"}
    ```

    **Field Descriptions:**

    - `status` - `pulling manifest`, `downloading <file>`, or `success`.
    - `digest` - Download lines: sha256: followed by the file name.
    - `completed` - Download lines: bytes of the file downloaded so far.
    - `total` - Download lines: the file's size in bytes.
    - `error` - Only on a failed download's last line.

=== "Non-streaming responses"

    `200`

    ```json
    {"status": "success"}
    ```
<!-- END GENERATED: ollama.pull -->

<!-- BEGIN GENERATED: ollama.embed -->
## `POST /api/embed`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns one embedding per input text, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Embedding model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to embed, or an array of texts. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

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

### Response format

`200`

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

**Field Descriptions:**

- `embeddings` - One vector per input, in input order.
- `total_duration`, `load_duration`, `prompt_eval_count` - Always 0.
<!-- END GENERATED: ollama.embed -->

<!-- BEGIN GENERATED: ollama.embeddings -->
## `POST /api/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the embedding of one text in Ollama's older format, loading the model on first use.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Embedding model to run. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text to embed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | No | Accepted in place of `prompt`. For an array, only the first text's embedding is returned. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

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

### Response format

`200`

```json
{
  "model": "nomic-embed-text-v1-GGUF",
  "embedding": [0.029926229268312454, 0.050356026738882065, -0.00871499814093113, -0.018384544178843498, 0.016114410012960434, 0.08822233229875565, -0.015040549449622631, -7.978447683854029e-05, ... 760 more]
}
```
<!-- END GENERATED: ollama.embeddings -->

<!-- BEGIN GENERATED: ollama.ps -->
## `GET /api/ps`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the loaded models in Ollama's format.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

[`GET /v1/health`](./lemonade.md#get-v1health) reports the loaded models in full.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/ps"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/ps
    ```

### Response format

`200`

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

**Field Descriptions:**

- `models`
  - `name` - Lemonade model name with a :latest tag.
  - `model` - Same as name.
  - `size` - Always 0.
  - `digest` - Always an all-zero sha256.
  - `details` - Inferred from the model name, recipe and checkpoint.
    - `parent_model` - Always empty.
    - `format` - Always gguf.
    - `family` - The model's recipe, e.g. llamacpp.
    - `parameter_size` - From the model name, e.g. 0.6B; empty when the name has none.
    - `quantization_level` - From the checkpoint, e.g. Q4_0; empty when the checkpoint has none.
  - `expires_at` - Always 2099-01-01T00:00:00Z.
  - `size_vram` - Always 0.
<!-- END GENERATED: ollama.ps -->

<!-- BEGIN GENERATED: ollama.version -->
## `GET /api/version`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports an Ollama version recent enough for Ollama clients' version checks. [`GET /v1/health`](./lemonade.md#get-v1health) reports Lemonade's own version.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/version"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/version
    ```

### Response format

`200`

```json
{"version": "0.16.1"}
```
<!-- END GENERATED: ollama.version -->

<!-- BEGIN GENERATED: ollama.create -->
## `POST /api/create`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported. Returns 501. Register a model with [`POST /v1/pull`](./lemonade.md#post-v1pull) instead.

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.create -->

<!-- BEGIN GENERATED: ollama.copy -->
## `POST /api/copy`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported. Returns 501. A model alias gives a model a second name instead; see [`POST /internal/aliases`](./internal.md#post-internalaliases).

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.copy -->

<!-- BEGIN GENERATED: ollama.push -->
## `POST /api/push`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported. Returns 501.

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.push -->

<!-- BEGIN GENERATED: ollama.blobs -->
## `POST /api/blobs/{digest}`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported. Returns 501.

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.blobs -->
