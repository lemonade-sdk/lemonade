# Ollama-Compatible API

Lemonade supports the [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), allowing applications built for Ollama to work with Lemonade without modification.

To enable auto-detection by Ollama-integrated apps, configure the server to use the Ollama default port `11434`. See [Server Configuration](../guide/configuration/README.md#settings-reference) for how to change the port.

<!-- BEGIN GENERATED: ollama.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/api/chat`](#post-apichat) | Chat completion, streaming and non-streaming |
| `POST` | [`/api/generate`](#post-apigenerate) | Text completion, or image generation for image models |
| `GET` | [`/api/tags`](#get-apitags) | List downloaded models |
| `POST` | [`/api/show`](#post-apishow) | Model details |
| `DELETE` | [`/api/delete`](#delete-apidelete) | Delete a model |
| `POST` | [`/api/pull`](#post-apipull) | Download a model, with progress |
| `POST` | [`/api/embed`](#post-apiembed) | Embeddings |
| `POST` | [`/api/embeddings`](#post-apiembeddings) | Legacy embeddings |
| `GET` | [`/api/ps`](#get-apips) | List running models |
| `GET` | [`/api/version`](#get-apiversion) | Version |
| `POST` | [`/api/create`](#post-apicreate) | Create a model from a Modelfile (not supported) |
| `POST` | [`/api/copy`](#post-apicopy) | Copy a model (not supported) |
| `POST` | [`/api/push`](#post-apipush) | Push a model to a registry (not supported) |
| `POST` | [`/api/blobs/{digest}`](#post-apiblobsdigest) | Upload a blob (not supported) |
<!-- END GENERATED: ollama.summary -->

<!-- BEGIN GENERATED: ollama.chat -->
## `POST /api/chat`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Generates the next assistant message for a conversation, loading the model on first use. Lemonade runs it as an OpenAI chat completion and converts the result.

Ollama streams by default: without `"stream": false` the response is newline-delimited JSON.

A request with tools and streaming runs non-streaming on the backend and answers with two NDJSON lines: the whole message, then the final line.

An empty `messages` array with `"keep_alive": 0` unloads the model instead, answering with `done_reason: "unload"`.

An unknown model answers `404` with `model '<name>' not found, try pulling it first`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run; loaded on first use. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `messages` | Yes | Conversation so far. Each message has a `role` and `content`, plus optional `images` (base64 strings), `tool_calls`, and, on `tool` messages, `tool_name` or `tool_call_id`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream newline-delimited JSON as tokens are generated. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | Sampling options: `temperature`, `top_p`, `seed`, `stop`, `num_predict` (maximum generated tokens) and `repeat_penalty`. These keys are also accepted at the top level, where they win. `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools` | No | Tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `format` | No | `json` constrains the reply to a JSON object. JSON schemas are not supported. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `think` | No | Not applied: reasoning models think regardless. Qwen3 models skip their reasoning when the prompt ends with `/no_think`. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `keep_alive` | No | `0` with an empty `messages` array unloads the model. Other values are ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/chat" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "stream": false
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/chat \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "stream": false
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "created_at": "2024-01-01T00:00:00Z",
      "done": true,
      "done_reason": "stop",
      "eval_count": 14,
      "eval_duration": 39445000,
      "load_duration": 0,
      "message": {"content": "The capital of France is **Paris**.", "role": "assistant"},
      "model": "Qwen3-0.6B-GGUF",
      "prompt_eval_count": 19,
      "prompt_eval_duration": 9895000,
      "total_duration": 0
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model", "created_at", "done"],
      "properties": {
        "created_at": {"description": "Always 2024-01-01T00:00:00Z.", "type": "string"},
        "done": {"type": "boolean"},
        "done_reason": {
          "description": "stop, length, tool_calls, or unload after an unload request.",
          "type": "string"
        },
        "eval_count": {"description": "Generated tokens.", "type": "integer"},
        "eval_duration": {
          "description": "Generation time in nanoseconds, when the backend reports timings; otherwise 0.",
          "type": "integer"
        },
        "load_duration": {"description": "Always 0.", "type": "integer"},
        "message": {
          "type": "object",
          "required": ["role", "content"],
          "properties": {
            "content": {"type": "string"},
            "role": {"type": "string"},
            "thinking": {
              "description": "Reasoning models only: the model's thinking.",
              "type": "string"
            },
            "tool_calls": {
              "description": "Calls to the request's tools, with arguments as objects.",
              "type": "array"
            }
          }
        },
        "model": {"type": "string"},
        "prompt_eval_count": {"description": "Prompt tokens.", "type": "integer"},
        "prompt_eval_duration": {
          "description": "Prompt processing time in nanoseconds, when the backend reports timings; otherwise 0.",
          "type": "integer"
        },
        "total_duration": {"description": "Always 0.", "type": "integer"}
      }
    }
    ```

### Response: `JsonLines`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/chat" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "stream": true
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/chat \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "messages": [{"content": "What is the capital of France? /no_think", "role": "user"}],
          "stream": true
        }'
    ```

=== "Response"

    `200`

    ```text
    {"created_at": "2024-01-01T00:00:00Z", "done": false, "message": {"content": "", "role": "assistant"}, "model": "Qwen3-0.6B-GGUF"}
    {"created_at": "2024-01-01T00:00:00Z", "done": false, "message": {"content": "The", "role": "assistant"}, "model": "Qwen3-0.6B-GGUF"}
    {"created_at": "2024-01-01T00:00:00Z", "done": false, "message": {"content": " capital", "role": "assistant"}, "model": "Qwen3-0.6B-GGUF"}
    ...
    {"created_at": "2024-01-01T00:00:00Z", "done": true, "done_reason": "stop", "message": {"content": "", "role": "assistant"}, "model": "Qwen3-0.6B-GGUF"}
    {"created_at": "2024-01-01T00:00:00Z", "done": true, "done_reason": "stop", "eval_count": 0, "eval_duration": 0, "load_duration": 0, "message": {"content": "", "role": "assistant"}, "model": "Qwen3-0.6B-GGUF", "prompt_eval_count": 0, "prompt_eval_duration": 0, "total_duration": 0}
    ```

=== "Schema"

    ```json
    {
      "description": "Each line carries the next message piece with done=false. When the message ends, a line with done=true and the real done_reason follows, then a last done=true line with the token counts, which are 0 when the backend reports no usage, and done_reason stop.",
      "type": "object",
      "required": ["model", "created_at", "done"],
      "properties": {
        "created_at": {"description": "Always 2024-01-01T00:00:00Z.", "type": "string"},
        "done": {"type": "boolean"},
        "done_reason": {
          "description": "stop, length, tool_calls, or unload after an unload request.",
          "type": "string"
        },
        "eval_count": {"description": "Generated tokens.", "type": "integer"},
        "eval_duration": {
          "description": "Generation time in nanoseconds, when the backend reports timings; otherwise 0.",
          "type": "integer"
        },
        "load_duration": {"description": "Always 0.", "type": "integer"},
        "message": {
          "type": "object",
          "required": ["role", "content"],
          "properties": {
            "content": {"type": "string"},
            "role": {"type": "string"},
            "thinking": {
              "description": "Reasoning models only: the model's thinking.",
              "type": "string"
            },
            "tool_calls": {
              "description": "Calls to the request's tools, with arguments as objects.",
              "type": "array"
            }
          }
        },
        "model": {"type": "string"},
        "prompt_eval_count": {"description": "Prompt tokens.", "type": "integer"},
        "prompt_eval_duration": {
          "description": "Prompt processing time in nanoseconds, when the backend reports timings; otherwise 0.",
          "type": "integer"
        },
        "total_duration": {"description": "Always 0.", "type": "integer"}
      }
    }
    ```
<!-- END GENERATED: ollama.chat -->

<!-- BEGIN GENERATED: ollama.generate -->
## `POST /api/generate`
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>

Continues a prompt, loading the model on first use. Lemonade runs it as an OpenAI text completion and converts the result. Naming an image model generates an image instead.

Ollama streams by default: without `"stream": false` the response is newline-delimited JSON. Image generation never streams.

An empty `prompt` with `"keep_alive": 0` unloads the model instead, answering with `done_reason: "unload"`.

An unknown model answers `404` with `model '<name>' not found, try pulling it first`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to run; loaded on first use. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text to continue, or the image to generate for an image model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream newline-delimited JSON as tokens are generated. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | Sampling options: `temperature`, `top_p`, `seed`, `stop`, `num_predict` (maximum generated tokens) and `repeat_penalty`. These keys are also accepted at the top level, where they win. `num_ctx` sets the context size when this request loads the model. Image models read `width`, `height`, `steps`, `cfg_scale` and `seed` here. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `width` | No | Image models only: image width in pixels. Defaults to 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `height` | No | Image models only: image height in pixels. Defaults to 512. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `steps` | No | Image models only: inference steps. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `cfg_scale` | No | Image models only: classifier-free guidance scale. Defaults to the model's own. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `seed` | No | Random seed for reproducible output. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `keep_alive` | No | `0` with an empty `prompt` unloads the model. Other values are ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `system` | No | System prompt. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `format` | No | Output format constraint. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/generate" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "stream": false,
          "options": {"num_predict": 16}
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/generate \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "stream": false,
          "options": {"num_predict": 16}
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "context": [],
      "created_at": "2024-01-01T00:00:00Z",
      "done": true,
      "done_reason": "stop",
      "eval_count": 16,
      "eval_duration": 0,
      "load_duration": 0,
      "model": "Qwen3-0.6B-GGUF",
      "prompt_eval_count": 5,
      "prompt_eval_duration": 0,
      "response": " Paris, and the capital of France is also the capital of the United States,",
      "total_duration": 0
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model", "created_at", "done"],
      "properties": {
        "context": {"description": "Always empty.", "type": "array"},
        "created_at": {"description": "Always 2024-01-01T00:00:00Z.", "type": "string"},
        "done": {"type": "boolean"},
        "done_reason": {
          "description": "stop, length, or unload after an unload request.",
          "type": "string"
        },
        "eval_count": {"description": "Generated tokens.", "type": "integer"},
        "eval_duration": {"description": "Always 0.", "type": "integer"},
        "image": {
          "description": "Image models only: the generated image, base64-encoded.",
          "type": "string"
        },
        "load_duration": {"description": "Always 0.", "type": "integer"},
        "model": {"type": "string"},
        "prompt_eval_count": {"description": "Prompt tokens.", "type": "integer"},
        "prompt_eval_duration": {"description": "Always 0.", "type": "integer"},
        "response": {"description": "Generated text. Empty for an image model.", "type": "string"},
        "total_duration": {"description": "Always 0.", "type": "integer"}
      }
    }
    ```

### Response: `JsonLines`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/generate" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "stream": true,
          "options": {"num_predict": 16}
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/generate \
      -H "Content-Type: application/json" \
      -d '{
          "model": "Qwen3-0.6B-GGUF",
          "prompt": "The capital of France is",
          "stream": true,
          "options": {"num_predict": 16}
        }'
    ```

=== "Response"

    `200`

    ```text
    {"created_at": "2024-01-01T00:00:00Z", "done": false, "model": "Qwen3-0.6B-GGUF", "response": " Paris"}
    {"created_at": "2024-01-01T00:00:00Z", "done": false, "model": "Qwen3-0.6B-GGUF", "response": "."}
    {"created_at": "2024-01-01T00:00:00Z", "done": false, "model": "Qwen3-0.6B-GGUF", "response": " Which"}
    ...
    {"created_at": "2024-01-01T00:00:00Z", "done": true, "done_reason": "length", "model": "Qwen3-0.6B-GGUF", "response": ""}
    {"context": [], "created_at": "2024-01-01T00:00:00Z", "done": true, "done_reason": "stop", "eval_count": 16, "eval_duration": 0, "load_duration": 0, "model": "Qwen3-0.6B-GGUF", "prompt_eval_count": 5, "prompt_eval_duration": 0, "response": "", "total_duration": 0}
    ```

=== "Schema"

    ```json
    {
      "description": "Each line carries the next piece of text with done=false. When the text ends, a line with done=true and the real done_reason follows, then a last done=true line with the token counts and done_reason stop.",
      "type": "object",
      "required": ["model", "created_at", "done"],
      "properties": {
        "context": {"description": "Always empty.", "type": "array"},
        "created_at": {"description": "Always 2024-01-01T00:00:00Z.", "type": "string"},
        "done": {"type": "boolean"},
        "done_reason": {
          "description": "stop, length, or unload after an unload request.",
          "type": "string"
        },
        "eval_count": {"description": "Generated tokens.", "type": "integer"},
        "eval_duration": {"description": "Always 0.", "type": "integer"},
        "image": {
          "description": "Image models only: the generated image, base64-encoded.",
          "type": "string"
        },
        "load_duration": {"description": "Always 0.", "type": "integer"},
        "model": {"type": "string"},
        "prompt_eval_count": {"description": "Prompt tokens.", "type": "integer"},
        "prompt_eval_duration": {"description": "Always 0.", "type": "integer"},
        "response": {"description": "Generated text. Empty for an image model.", "type": "string"},
        "total_duration": {"description": "Always 0.", "type": "integer"}
      }
    }
    ```
<!-- END GENERATED: ollama.generate -->

<!-- BEGIN GENERATED: ollama.tags -->
## `GET /api/tags`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the downloaded models in Ollama's format.

Every name carries a `:latest` tag, `modified_at` and `digest` are fixed placeholders, and `size` is the registry's size estimate in bytes.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/tags"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/tags
    ```

=== "Response"

    `200`

    ```json
    {
      "models": [
        {
          "details": {
            "families": ["onnxruntime"],
            "family": "onnxruntime",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "Phishing-Email-Detection-ONNX:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "Phishing-Email-Detection-ONNX:latest",
          "size": 268435456
        },
        {
          "details": {
            "families": ["llamacpp"],
            "family": "llamacpp",
            "format": "gguf",
            "parameter_size": "0.6B",
            "parent_model": "",
            "quantization_level": "Q4_0"
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "Qwen3-0.6B-GGUF:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "Qwen3-0.6B-GGUF:latest",
          "size": 382252089
        },
        {
          "details": {
            "families": ["llamacpp-hrx"],
            "family": "llamacpp-hrx",
            "format": "gguf",
            "parameter_size": "0.6B",
            "parent_model": "",
            "quantization_level": "Q4_0"
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "Qwen3-0.6B-HRX:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "Qwen3-0.6B-HRX:latest",
          "size": 382252089
        },
        {
          "details": {
            "families": ["sd-cpp"],
            "family": "sd-cpp",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "RealESRGAN-x4plus:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "RealESRGAN-x4plus:latest",
          "size": 66571993
        },
        {
          "details": {
            "families": ["sd-cpp"],
            "family": "sd-cpp",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "SD-Turbo:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "SD-Turbo:latest",
          "size": 5218385264
        },
        {
          "details": {
            "families": ["trellis"],
            "family": "trellis",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "TRELLIS-3D:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "TRELLIS-3D:latest",
          "size": 32963873996
        },
        {
          "details": {
            "families": ["thinksound"],
            "family": "thinksound",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "ThinkSound-SFX:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "ThinkSound-SFX:latest",
          "size": 6871947673
        },
        {
          "details": {
            "families": ["whispercpp"],
            "family": "whispercpp",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "Whisper-Tiny:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "Whisper-Tiny:latest",
          "size": 77309411
        },
        {
          "details": {
            "families": ["llamacpp"],
            "family": "llamacpp",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": "Q8_0"
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "jina-reranker-v1-tiny-en-GGUF:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "jina-reranker-v1-tiny-en-GGUF:latest",
          "size": 36507222
        },
        {
          "details": {
            "families": ["kokoro"],
            "family": "kokoro",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": ""
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "kokoro-v1:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "kokoro-v1:latest",
          "size": 0
        },
        {
          "details": {
            "families": ["llamacpp"],
            "family": "llamacpp",
            "format": "gguf",
            "parameter_size": "",
            "parent_model": "",
            "quantization_level": "Q4_K_S"
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "model": "nomic-embed-text-v1-GGUF:latest",
          "modified_at": "2024-01-01T00:00:00Z",
          "name": "nomic-embed-text-v1-GGUF:latest",
          "size": 78383153
        }
      ]
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["models"],
      "properties": {
        "models": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["name", "model", "modified_at", "size", "digest", "details"],
            "properties": {
              "details": {
                "description": "Inferred from the model name, recipe and checkpoint.",
                "type": "object",
                "properties": {
                  "families": {"type": "array", "items": {"type": "string"}},
                  "family": {"description": "The model's recipe, e.g. llamacpp.", "type": "string"},
                  "format": {"description": "Always gguf.", "type": "string"},
                  "parameter_size": {
                    "description": "From the model name, e.g. 0.6B; empty when the name has none.",
                    "type": "string"
                  },
                  "parent_model": {"description": "Always empty.", "type": "string"},
                  "quantization_level": {
                    "description": "From the checkpoint, e.g. Q4_0; empty when the checkpoint has none.",
                    "type": "string"
                  }
                }
              },
              "digest": {"description": "Always an all-zero sha256.", "type": "string"},
              "model": {"description": "Same as name.", "type": "string"},
              "modified_at": {"description": "Always 2024-01-01T00:00:00Z.", "type": "string"},
              "name": {"description": "Lemonade model name with a :latest tag.", "type": "string"},
              "size": {
                "description": "Size in bytes, from the registry's estimate.",
                "type": "integer"
              }
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: ollama.tags -->

<!-- BEGIN GENERATED: ollama.show -->
## `POST /api/show`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Describes one registered model in Ollama's format.

`capabilities` follows the model's labels: `completion` for chat models, `embedding` for embedding models, plus `tools`, `vision` and `thinking` when the model carries the matching label.

An unknown model answers `404` with `model '<name>' not found`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to describe. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `name` | No | Older name for `model`; wins when both are given. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
      "capabilities": ["completion", "tools", "thinking"],
      "details": {
        "families": ["llamacpp"],
        "family": "llamacpp",
        "format": "gguf",
        "parameter_size": "0.6B",
        "parent_model": "",
        "quantization_level": "Q4_0"
      },
      "model_info": {
        "general.architecture": "llamacpp",
        "general.file_type": 0,
        "general.parameter_count": 0,
        "general.quantization_version": 0,
        "llamacpp.context_length": -1
      },
      "modelfile": "# Modelfile generated by Lemonade\nFROM unsloth/Qwen3-0.6B-GGUF:Q4_0",
      "parameters": "num_ctx -1",
      "template": ""
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["modelfile", "parameters", "template", "details", "capabilities", "model_info"],
      "properties": {
        "capabilities": {
          "type": "array",
          "items": {"enum": ["completion", "embedding", "tools", "vision", "thinking"]}
        },
        "details": {
          "description": "Inferred from the model name, recipe and checkpoint.",
          "type": "object",
          "properties": {
            "families": {"type": "array", "items": {"type": "string"}},
            "family": {"description": "The model's recipe, e.g. llamacpp.", "type": "string"},
            "format": {"description": "Always gguf.", "type": "string"},
            "parameter_size": {
              "description": "From the model name, e.g. 0.6B; empty when the name has none.",
              "type": "string"
            },
            "parent_model": {"description": "Always empty.", "type": "string"},
            "quantization_level": {
              "description": "From the checkpoint, e.g. Q4_0; empty when the checkpoint has none.",
              "type": "string"
            }
          }
        },
        "model_info": {
          "description": "general.architecture (the recipe) and <recipe>.context_length; the other keys are always 0.",
          "type": "object"
        },
        "modelfile": {"description": "A FROM line naming the checkpoint.", "type": "string"},
        "parameters": {
          "description": "num_ctx followed by the model's configured context size.",
          "type": "string"
        },
        "template": {"description": "Always empty.", "type": "string"}
      }
    }
    ```
<!-- END GENERATED: ollama.show -->

<!-- BEGIN GENERATED: ollama.delete -->
## `DELETE /api/delete`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Deletes a downloaded model's files, unloading it first when it is loaded.

Success answers `200` with an empty body, as Ollama does. An unknown or unsupported model answers `404`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to delete. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `name` | No | Older name for `model`; wins when both are given. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Empty`

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

=== "Response"

    `200`
<!-- END GENERATED: ollama.delete -->

<!-- BEGIN GENERATED: ollama.pull -->
## `POST /api/pull`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Downloads a registered model, or updates it when its registry has a newer version.

Only models already in Lemonade's registry can be pulled; use [`POST /v1/pull`](./lemonade.md#post-v1pull) to register a new one. An unknown model answers `404` with `model '<name>' not found`.

Ollama streams by default: without `"stream": false` the response is newline-delimited JSON progress. A failed download ends the stream with an `error` line.

In offline mode the request answers `400` with code `lemond_offline`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Model to download. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `name` | No | Older name for `model`; wins when both are given. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | Stream newline-delimited JSON progress. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `JsonLines`

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

=== "Response"

    `200`

    ```text
    {"status": "pulling manifest"}
    {"completed": 0, "digest": "sha256:Qwen3-0.6B-Q4_0.gguf", "status": "downloading Qwen3-0.6B-Q4_0.gguf", "total": 382156480}
    {"completed": 382156480, "digest": "sha256:Qwen3-0.6B-Q4_0.gguf", "status": "downloading Qwen3-0.6B-Q4_0.gguf", "total": 382156480}
    ...
    {"status": "success"}
    {"status": "success"}
    ```

=== "Schema"

    ```json
    {
      "description": "The first line is pulling manifest, then one line per progress report, then success.",
      "type": "object",
      "properties": {
        "completed": {
          "description": "Download lines: bytes of the file downloaded so far.",
          "type": "integer"
        },
        "digest": {
          "description": "Download lines: sha256: followed by the file name.",
          "type": "string"
        },
        "error": {"description": "Only on a failed download's last line.", "type": "string"},
        "status": {
          "description": "pulling manifest, downloading <file>, or success.",
          "type": "string"
        },
        "total": {"description": "Download lines: the file's size in bytes.", "type": "integer"}
      }
    }
    ```

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {"status": "success"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["status"], "properties": {"status": {"const": "success"}}}
    ```
<!-- END GENERATED: ollama.pull -->

<!-- BEGIN GENERATED: ollama.embed -->
## `POST /api/embed`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns one embedding per input text, loading the model on first use.

An unknown model answers `404` with `model '<name>' not found`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Embedding model to run; loaded on first use. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | Yes | Text to embed, or an array of texts. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
      "embeddings": [
        [0.029926229268312454, 0.050356026738882065, -0.00871499814093113, -0.018384544178843498, 0.016114410012960434, 0.08822233229875565, -0.015040549449622631, -7.978447683854029e-05, ... 760 more]
      ],
      "load_duration": 0,
      "model": "nomic-embed-text-v1-GGUF",
      "prompt_eval_count": 0,
      "total_duration": 0
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model", "embeddings"],
      "properties": {
        "embeddings": {
          "description": "One vector per input, in input order.",
          "type": "array",
          "items": {"type": "array", "items": {"type": "number"}}
        },
        "load_duration": {"description": "Always 0.", "type": "integer"},
        "model": {"type": "string"},
        "prompt_eval_count": {"description": "Always 0.", "type": "integer"},
        "total_duration": {"description": "Always 0.", "type": "integer"}
      }
    }
    ```
<!-- END GENERATED: ollama.embed -->

<!-- BEGIN GENERATED: ollama.embeddings -->
## `POST /api/embeddings`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the embedding of one text in Ollama's older format, loading the model on first use.

Only the first input's embedding is returned. An unknown model answers `404` with `model '<name>' not found`.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Embedding model to run; loaded on first use. A `:latest` tag is ignored. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | Yes | Text to embed. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `input` | No | Accepted in place of `prompt`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `options` | No | `num_ctx` sets the context size when this request loads the model. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Wins over `options.num_ctx`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

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

=== "Response"

    `200`

    ```json
    {
      "embedding": [0.029926229268312454, 0.050356026738882065, -0.00871499814093113, -0.018384544178843498, 0.016114410012960434, 0.08822233229875565, -0.015040549449622631, -7.978447683854029e-05, ... 760 more],
      "model": "nomic-embed-text-v1-GGUF"
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model", "embedding"],
      "properties": {
        "embedding": {"type": "array", "items": {"type": "number"}},
        "model": {"type": "string"}
      }
    }
    ```
<!-- END GENERATED: ollama.embeddings -->

<!-- BEGIN GENERATED: ollama.ps -->
## `GET /api/ps`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Lists the loaded models in Ollama's format.

`size` and `size_vram` are always 0 and `expires_at` is a fixed far-future date. [`GET /v1/health`](./lemonade.md#get-v1health) reports the loaded models in full.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/ps"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/ps
    ```

=== "Response"

    `200`

    ```json
    {
      "models": [
        {
          "details": {
            "families": ["llamacpp"],
            "family": "llamacpp",
            "format": "gguf",
            "parameter_size": "0.6B",
            "parent_model": "",
            "quantization_level": "Q4_0"
          },
          "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
          "expires_at": "2099-01-01T00:00:00Z",
          "model": "Qwen3-0.6B-GGUF:latest",
          "name": "Qwen3-0.6B-GGUF:latest",
          "size": 0,
          "size_vram": 0
        }
      ]
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["models"],
      "properties": {
        "models": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["name", "model", "size", "digest", "details", "expires_at", "size_vram"],
            "properties": {
              "details": {
                "description": "Inferred from the model name, recipe and checkpoint.",
                "type": "object",
                "properties": {
                  "families": {"type": "array", "items": {"type": "string"}},
                  "family": {"description": "The model's recipe, e.g. llamacpp.", "type": "string"},
                  "format": {"description": "Always gguf.", "type": "string"},
                  "parameter_size": {
                    "description": "From the model name, e.g. 0.6B; empty when the name has none.",
                    "type": "string"
                  },
                  "parent_model": {"description": "Always empty.", "type": "string"},
                  "quantization_level": {
                    "description": "From the checkpoint, e.g. Q4_0; empty when the checkpoint has none.",
                    "type": "string"
                  }
                }
              },
              "digest": {"description": "Always an all-zero sha256.", "type": "string"},
              "expires_at": {"description": "Always 2099-01-01T00:00:00Z.", "type": "string"},
              "model": {"description": "Same as name.", "type": "string"},
              "name": {"description": "Lemonade model name with a :latest tag.", "type": "string"},
              "size": {"description": "Always 0.", "type": "integer"},
              "size_vram": {"description": "Always 0.", "type": "integer"}
            }
          }
        }
      }
    }
    ```
<!-- END GENERATED: ollama.ps -->

<!-- BEGIN GENERATED: ollama.version -->
## `GET /api/version`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reports an Ollama version recent enough for Ollama clients' version checks. [`GET /v1/health`](./lemonade.md#get-v1health) reports Lemonade's own version.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/api/version"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/api/version
    ```

=== "Response"

    `200`

    ```json
    {"version": "0.16.1"}
    ```

=== "Schema"

    ```json
    {"type": "object", "required": ["version"], "properties": {"version": {"type": "string"}}}
    ```
<!-- END GENERATED: ollama.version -->

<!-- BEGIN GENERATED: ollama.create -->
## `POST /api/create`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported: answers `501`. Register a model with [`POST /v1/pull`](./lemonade.md#post-v1pull) instead.

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.create -->

<!-- BEGIN GENERATED: ollama.copy -->
## `POST /api/copy`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported: answers `501`. A model alias gives a model a second name instead; see [`POST /internal/aliases`](./internal.md#post-internalaliases).

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.copy -->

<!-- BEGIN GENERATED: ollama.push -->
## `POST /api/push`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported: answers `501`.

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.push -->

<!-- BEGIN GENERATED: ollama.blobs -->
## `POST /api/blobs/{digest}`
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>

Not supported: answers `501`.

Requires `LEMONADE_API_KEY` when it is set.
<!-- END GENERATED: ollama.blobs -->
