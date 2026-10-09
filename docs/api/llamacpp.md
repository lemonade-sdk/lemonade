# llama.cpp-Specific API

This page documents Lemonade's llama.cpp-specific compatibility surface. Lemonade forwards each of these requests to a llama.cpp server: rerank to the model it names, and slots and tokenize, which name no model, to the most recently used loaded model.

<!-- BEGIN GENERATED: llamacpp.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/rerank`](#post-v1rerank) | Score documents by relevance to a query |
| `GET` | [`/v1/slots`](#get-v1slots) | Processing state of the llama.cpp slots |
| `POST` | [`/v1/slots/{id}`](#post-v1slotsid) | Save, restore or erase the prompt cache of one slot |
| `POST` | [`/v1/tokenize`](#post-v1tokenize) | Tokenize text |
<!-- END GENERATED: llamacpp.summary -->

<!-- BEGIN GENERATED: llamacpp.rerank -->
## `POST /v1/rerank`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Scores each document by its relevance to a query, loading the reranking model on first use.

Lemonade forwards the request to llama.cpp's `/v1/rerank`, so only reranking models (the `reranking` label) using the `llamacpp` recipe, such as `bge-reranker-v2-m3-GGUF`, serve it. `/rerank` is the path most clients expect; `/reranking` and `/reranker` behave identically.

Results are returned in input order. To rank documents, sort `results` by `relevance_score`, highest first.

Also served at `/api/v0/rerank`, `/api/v1/rerank`, `/v0/rerank`, `/api/v0/reranking`, `/api/v1/reranking`, `/v0/reranking`, `/v1/reranking`, `/api/v0/reranker`, `/api/v1/reranker`, `/v0/reranker` and `/v1/reranker`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `model` | Yes | Reranking model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `query` | Yes | Search query the documents are scored against. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `documents` | Yes | Document strings to score against the query. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/rerank" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "jina-reranker-v1-tiny-en-GGUF",
          "query": "What is the capital of France?",
          "documents": [
            "Paris is the capital of France.",
            "Berlin is the capital of Germany.",
            "Madrid is the capital of Spain."
          ]
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/rerank \
      -H "Content-Type: application/json" \
      -d '{
          "model": "jina-reranker-v1-tiny-en-GGUF",
          "query": "What is the capital of France?",
          "documents": [
            "Paris is the capital of France.",
            "Berlin is the capital of Germany.",
            "Madrid is the capital of Spain."
          ]
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "model": "jina-reranker-v1-tiny-en-GGUF",
      "object": "list",
      "results": [
        {"index": 0, "relevance_score": 0.11989831924438477},
        {"index": 1, "relevance_score": 0.03523308038711548},
        {"index": 2, "relevance_score": 0.028406262397766113}
      ],
      "usage": {"prompt_tokens": 57, "total_tokens": 57}
    }
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["model", "object", "results"],
      "properties": {
        "model": {"description": "Model that scored the documents.", "type": "string"},
        "object": {"const": "list"},
        "results": {
          "description": "One entry per input document, in input order.",
          "type": "array",
          "items": {
            "type": "object",
            "required": ["index", "relevance_score"],
            "properties": {
              "index": {
                "description": "Position of the document in the request.",
                "type": "integer"
              },
              "relevance_score": {
                "description": "Relevance to the query; higher is more relevant.",
                "type": "number"
              }
            }
          }
        },
        "usage": {
          "type": "object",
          "properties": {
            "prompt_tokens": {"description": "Tokens in the input.", "type": "integer"},
            "total_tokens": {"description": "Tokens processed.", "type": "integer"}
          }
        }
      }
    }
    ```
<!-- END GENERATED: llamacpp.rerank -->

<!-- BEGIN GENERATED: llamacpp.slots -->
## `GET /v1/slots`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the state of every processing slot of a loaded llama.cpp model. Slots are parallel processing contexts, each able to serve one request at a time.

The request names no model: Lemonade forwards it to llama.cpp's `/slots` on the most recently used loaded model, which must be a llama.cpp model. With no model loaded, or when that model is not a llama.cpp model, the answer is `400`.

Also served at `/api/v0/slots`, `/api/v1/slots` and `/v0/slots`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint takes no parameters.

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/slots"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/slots
    ```

=== "Response"

    `200`

    ```json
    [{"id": 0, "is_processing": false, "n_ctx": 40960, "speculative": false}]
    ```

=== "Schema"

    ```json
    {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id"],
        "properties": {
          "id": {"description": "Slot id, used by POST /v1/slots/{id}.", "type": "integer"},
          "id_task": {
            "description": "Task the slot is serving, or -1 when idle.",
            "type": "integer"
          },
          "is_processing": {
            "description": "Whether the slot is serving a request.",
            "type": "boolean"
          },
          "n_ctx": {"description": "Context size of the slot.", "type": "integer"},
          "n_prompt_tokens": {
            "description": "Tokens in the slot's current prompt.",
            "type": "integer"
          },
          "next_token": {
            "description": "Generation state of the current request: has_next_token, n_decoded, n_remain.",
            "type": "array"
          },
          "params": {
            "description": "Sampling parameters of the slot's last request.",
            "type": "object"
          }
        }
      }
    }
    ```
<!-- END GENERATED: llamacpp.slots -->

<!-- BEGIN GENERATED: llamacpp.slots_action -->
## `POST /v1/slots/{id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Saves a slot's prompt cache to a file, restores it from one, or erases it, so a conversation's context can be persisted and resumed later.

The request names no model: Lemonade forwards it to llama.cpp's `/slots/{id}?action=...` on the most recently used loaded model, which must be a llama.cpp model. With no model loaded, or when that model is not a llama.cpp model, the answer is `400`. An `error` from llama.cpp, such as an unknown slot, is returned with its status.

llama.cpp answers every action with `501` unless the model was loaded with `--slot-save-path` in its llama.cpp arguments; see [Saving Slots](#saving-slots).

Also served at `/api/v0/slots/{id}`, `/api/v1/slots/{id}` and `/v0/slots/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | Slot id, from `GET /v1/slots`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `action` (query) | Yes | `save` writes the slot's prompt cache to `filename`, `restore` reads it back from `filename`, and `erase` clears the slot. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `filename` | No | Required for `save` and `restore`: the cache file, relative to llama.cpp's `--slot-save-path`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=erase" `
      -Method POST
    ```

=== "Bash"

    ```bash
    curl -X POST "http://localhost:13305/v1/slots/0?action=erase"
    ```

=== "Response"

    `200`

    ```json
    {"id_slot": 0, "n_erased": 0}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["id_slot"],
      "properties": {
        "filename": {"description": "save and restore: the cache file.", "type": "string"},
        "id_slot": {"description": "The slot acted on.", "type": "integer"},
        "n_erased": {"description": "erase: tokens cleared.", "type": "integer"},
        "n_read": {"description": "restore: bytes read.", "type": "integer"},
        "n_restored": {"description": "restore: tokens read from the file.", "type": "integer"},
        "n_saved": {"description": "save: tokens written to the file.", "type": "integer"},
        "n_written": {"description": "save: bytes written.", "type": "integer"},
        "timings": {
          "description": "save and restore: how long the file operation took.",
          "type": "object"
        }
      }
    }
    ```
<!-- END GENERATED: llamacpp.slots_action -->

### Saving Slots

llama.cpp saves and restores slot files only when the model was loaded with `--slot-save-path`, the directory those files are written to and read from. Set it for one load with `llamacpp_args`:

```bash
curl -X POST http://localhost:13305/v1/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen3-0.6B-GGUF",
    "llamacpp_args": "--slot-save-path /path/to/slot/saves"
  }'
```

Or set it for every llama.cpp model in the [server configuration](../guide/configuration/README.md):

```bash
lemonade config set llamacpp.args="--slot-save-path /path/to/slot/saves"
```

Save a slot's prompt cache, then restore it later. `filename` is relative to the `--slot-save-path` directory:

```bash
curl -X POST "http://localhost:13305/v1/slots/0?action=save" \
  -H "Content-Type: application/json" \
  -d '{"filename": "my_conversation_cache.bin"}'
```

```json
{"filename": "my_conversation_cache.bin", "id_slot": 0, "n_saved": 38, "n_written": 4359468, "timings": {"save_ms": 3.938}}
```

```bash
curl -X POST "http://localhost:13305/v1/slots/0?action=restore" \
  -H "Content-Type: application/json" \
  -d '{"filename": "my_conversation_cache.bin"}'
```

```json
{"filename": "my_conversation_cache.bin", "id_slot": 0, "n_read": 4359468, "n_restored": 38, "timings": {"restore_ms": 0.826}}
```

<!-- BEGIN GENERATED: llamacpp.tokenize -->
## `POST /v1/tokenize`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Tokenizes text with a loaded llama.cpp model's tokenizer, without using any of the model's context window.

The request names no model: Lemonade forwards it to llama.cpp's `/tokenize` on the most recently used loaded model, which must be a llama.cpp model. With no model loaded, or when that model is not a llama.cpp model, the answer is `400`. Models that do not share a tokenizer return different tokens for the same text, so after a request to a reranker, for example, the reranker's tokenizer answers.

Also served at `/api/v0/tokenize`, `/api/v1/tokenize` and `/v0/tokenize`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `content` | Yes | Text to tokenize. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `add_special` | No | Insert special tokens, such as `BOS`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `parse_special` | No | Tokenize special tokens; when `false`, they are treated as plain text. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `with_pieces` | No | Return each token as `{"id", "piece"}` instead of a bare id. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/tokenize" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"content": "This is a string to tokenize"}'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/tokenize \
      -H "Content-Type: application/json" \
      -d '{"content": "This is a string to tokenize"}'
    ```

=== "Response"

    `200`

    ```json
    {"tokens": [1986, 374, 264, 914, 311, 77651]}
    ```

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["tokens"],
      "properties": {
        "tokens": {
          "description": "Token ids, or {id, piece} objects when with_pieces is true.",
          "type": "array",
          "items": {"type": ["integer", "object"]}
        }
      }
    }
    ```
<!-- END GENERATED: llamacpp.tokenize -->
