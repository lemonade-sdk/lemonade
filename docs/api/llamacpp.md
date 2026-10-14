# llama.cpp-Specific API

This page documents Lemonade's llama.cpp-specific compatibility surface. Lemonade forwards each of these requests to a llama.cpp server: rerank to the model it names, and slots and tokenize, which name no model, to the most recently used loaded model.

## Summary

<!-- BEGIN GENERATED: llamacpp.summary -->
| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/rerank`](#post-v1rerank) | Reranking | query + documents -> relevance-scored documents |
| `GET` | [`/v1/slots`](#get-v1slots) | Returns the current slots processing state | slots state |
| `POST` | [`/v1/slots/{id}?action=save`](#post-v1slotsid) | Save the prompt cache of the specified slot to a file | prompt cache |
| `POST` | [`/v1/slots/{id}?action=restore`](#post-v1slotsid) | Restore the prompt cache of the specified slot from a file | prompt cache |
| `POST` | [`/v1/slots/{id}?action=erase`](#post-v1slotsid) | Erase the prompt cache of the specified slot | prompt cache |
| `POST` | [`/v1/tokenize`](#post-v1tokenize) | Tokenize a given text | tokenization |
<!-- END GENERATED: llamacpp.summary -->

<!-- BEGIN GENERATED: llamacpp.rerank -->
## `POST /v1/rerank`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reranking API for llama.cpp-compatible reranker models. You provide a query and a list of documents, and receive relevance scores for each document. Lemonade will load the requested model automatically if it is not already loaded.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/v1/rerank` endpoint.

> **Note:** Lemonade also accepts `/reranking` and `/reranker` as aliases — all three route to the same handler and behave identically.

> **Note:** The endpoint is available under all four path prefixes: `/api/v0/`, `/api/v1/`, `/v0/`, and `/v1/`.

> **Note:** This endpoint is only available for reranker-specific models using the `llamacpp` recipe, such as `bge-reranker-v2-m3-GGUF`.

Also served at `/api/v0/rerank`, `/api/v1/rerank`, `/v0/rerank`, `/api/v0/reranking`, `/api/v1/reranking`, `/v0/reranking`, `/v1/reranking`, `/api/v0/reranker`, `/api/v1/reranker`, `/v0/reranker` and `/v1/reranker`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` | Yes | The search query text. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `documents` | Yes | Array of document strings to score against the query. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The reranking model to use. If not already loaded, Lemonade loads it before forwarding the request. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/rerank" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "model": "bge-reranker-v2-m3-GGUF",
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
          "model": "bge-reranker-v2-m3-GGUF",
          "query": "What is the capital of France?",
          "documents": [
            "Paris is the capital of France.",
            "Berlin is the capital of Germany.",
            "Madrid is the capital of Spain."
          ]
        }'
    ```

### Response format

`200`

```json
{
  "model": "bge-reranker-v2-m3-GGUF",
  "object": "list",
  "results": [
    {"index": 0, "relevance_score": 8.634263038635254},
    {"index": 2, "relevance_score": -3.4511733055114746},
    {"index": 1, "relevance_score": -5.399629592895508}
  ],
  "usage": {"prompt_tokens": 51, "total_tokens": 51}
}
```

**Field Descriptions:**

- `model` - Model identifier used for reranking
- `object` - Type of response object, always `"list"`
- `results` - Array of all input documents with relevance scores, sorted by `relevance_score`, highest first
  - `index` - Original index of the document in the input array
  - `relevance_score` - Relevance score assigned by the model; higher means more relevant
- `usage` - Token usage statistics
  - `prompt_tokens` - Number of tokens in the input
  - `total_tokens` - Total tokens processed
<!-- END GENERATED: llamacpp.rerank -->

<!-- BEGIN GENERATED: llamacpp.slots -->
## `GET /v1/slots`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the current state of all processing slots in the llama.cpp server. Slots are parallel processing contexts that can handle multiple requests concurrently.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/slots` endpoint.

> **Note:** This endpoint is only available when a llama.cpp model is loaded.

> **Note:** This endpoint supports all four path prefixes: `/api/v0/slots`, `/api/v1/slots`, `/v0/slots`, and `/v1/slots`.

Also served at `/api/v0/slots`, `/api/v1/slots` and `/v0/slots`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

The request names no model: Lemonade forwards it to llama.cpp's `/slots` on the most recently used loaded model, which must be a llama.cpp model. With no model loaded, or when that model is not a llama.cpp model, the answer is `400`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/slots"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/slots
    ```

### Response format

`200`

```json
[{"id": 0, "n_ctx": 40960, "is_processing": false, "speculative": false}]
```

**Field Descriptions:**

- `id` - Unique identifier for the slot
- `next_token` - Information about token generation state
  - `has_next_token` - Whether more tokens are expected
  - `n_remain` - Number of tokens remaining to generate
  - `n_decoded` - Number of tokens already decoded
- `id_task` - Identifier of the current task being processed (the last task processed if idle)
- `n_ctx` - Context size of the slot.
- `is_processing` - Whether the slot is serving a request.
- `n_prompt_tokens` - Tokens in the slot's current prompt.
- `params` - Sampling parameters of the slot's last request.
<!-- END GENERATED: llamacpp.slots -->

<!-- BEGIN GENERATED: llamacpp.slots_action -->
## `POST /v1/slots/{id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Save the prompt cache of a specific slot to a file. This allows you to persist the current context state for later restoration.

Restore the prompt cache of a specific slot from a previously saved file. This allows you to resume a conversation or context from where you left off.

Erase (clear) the prompt cache of a specific slot. This removes all cached context from the slot, resetting it to an empty state.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/slots/{id}?action=save`, `/slots/{id}?action=restore` or `/slots/{id}?action=erase` endpoint.

> **Note:** The llama.cpp server must be started with the `--slot-save-path` argument for save, restore and erase operations to work. See [Server Configuration](../guide/configuration/README.md) for details on configuring backend arguments.
>
> Example configuration:
> ```bash
> lemonade config set llamacpp.args="--slot-save-path /path/to/slot/saves"
> ```

> **Note:** This endpoint supports all four path prefixes: `/api/v0/slots/{id}`, `/api/v1/slots/{id}`, `/v0/slots/{id}`, and `/v1/slots/{id}`.

Also served at `/api/v0/slots/{id}`, `/api/v1/slots/{id}` and `/v0/slots/{id}`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | The slot ID to save, restore to or erase (path parameter). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `filename` | No | The filename where the slot cache should be saved, or from which to restore the slot cache (JSON body). Required for `save` and `restore`, relative to llama.cpp's `--slot-save-path`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `action` (query) | Yes | `save` writes the slot's prompt cache to `filename`, `restore` reads it back from `filename`, and `erase` clears the slot. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The request names no model: Lemonade sends it to the most recently used loaded model, which must be a llama.cpp model. With no model loaded, or when that model is not a llama.cpp model, the answer is `400`. An `error` from llama.cpp is returned with its status.

Without `--slot-save-path`, llama.cpp answers `501`. Besides the server configuration, [`POST /v1/load`](./lemonade.md#post-v1load) can set it for one load with `llamacpp_args`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=save" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}'
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/slots/0?action=save" \
      -H "Content-Type: application/json" \
      -d '{"filename": "my_conversation_cache.bin"}'
    ```

=== "PowerShell (restore)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=restore" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}'
    ```

=== "Bash (restore)"

    ```bash
    curl "http://localhost:13305/v1/slots/0?action=restore" \
      -H "Content-Type: application/json" \
      -d '{"filename": "my_conversation_cache.bin"}'
    ```

=== "PowerShell (erase)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=erase" `
      -Method POST
    ```

=== "Bash (erase)"

    ```bash
    curl -X POST "http://localhost:13305/v1/slots/0?action=erase"
    ```

### Response format

=== "Save responses"

    `200`

    ```json
    {
      "id_slot": 0,
      "filename": "my_conversation_cache.bin",
      "n_saved": 307,
      "n_written": 35214844,
      "timings": {"save_ms": 10.769}
    }
    ```

    **Field Descriptions:**

    - `id_slot` - The slot ID that was saved, restored or erased
    - `filename` - The filename where the cache was saved, or from which the cache was restored
    - `n_saved` - Number of tokens saved to the cache file
    - `n_restored` - Number of tokens restored from the cache file
    - `n_written` - save: bytes written.
    - `n_read` - restore: bytes read.
    - `n_erased` - erase: tokens cleared.
    - `timings` - save and restore: how long the file operation took.

=== "Restore responses"

    `200`

    ```json
    {
      "id_slot": 0,
      "filename": "my_conversation_cache.bin",
      "n_restored": 231,
      "n_read": 26497340,
      "timings": {"restore_ms": 3.478}
    }
    ```

    **Field Descriptions:**

    - `id_slot` - The slot ID that was saved, restored or erased
    - `filename` - The filename where the cache was saved, or from which the cache was restored
    - `n_saved` - Number of tokens saved to the cache file
    - `n_restored` - Number of tokens restored from the cache file
    - `n_written` - save: bytes written.
    - `n_read` - restore: bytes read.
    - `n_erased` - erase: tokens cleared.
    - `timings` - save and restore: how long the file operation took.

=== "Erase responses"

    `200`

    ```json
    {"id_slot": 0, "n_erased": 0}
    ```

    **Field Descriptions:**

    - `id_slot` - The slot ID that was saved, restored or erased
    - `filename` - The filename where the cache was saved, or from which the cache was restored
    - `n_saved` - Number of tokens saved to the cache file
    - `n_restored` - Number of tokens restored from the cache file
    - `n_written` - save: bytes written.
    - `n_read` - restore: bytes read.
    - `n_erased` - erase: tokens cleared.
    - `timings` - save and restore: how long the file operation took.
<!-- END GENERATED: llamacpp.slots_action -->

<!-- BEGIN GENERATED: llamacpp.tokenize -->
## `POST /v1/tokenize`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Tokenize a given text. Does not count towards the current model's context window.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/tokenize` endpoint.

> **Note:** This endpoint supports all four path prefixes: `/api/v0/tokenize`, `/api/v1/tokenize`, `/v0/tokenize`, and `/v1/tokenize`.

> **Note:** Actual response values may vary for the same string across different models if the models do not share the same tokenizer.

Also served at `/api/v0/tokenize`, `/api/v1/tokenize` and `/v0/tokenize`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `content` | Yes | The text to tokenize. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `add_special` | No | Boolean indicating if special tokens, i.e. `BOS`, should be inserted. Default: `false` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `parse_special` | No | Boolean indicating if special tokens should be tokenized. When `false` special tokens are treated as plaintext. Default: `true` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `with_pieces` | No | Boolean indicating whether to return token pieces along with IDs. Default: `false` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

The request names no model: Lemonade forwards it to llama.cpp's `/tokenize` on the most recently used loaded model, which must be a llama.cpp model, so after a request to a reranker, for example, the reranker's tokenizer answers. With no model loaded, or when that model is not a llama.cpp model, the answer is `400`.

### Example request

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

### Response format

`200`

```json
{"tokens": [1986, 374, 264, 914, 311, 77651]}
```

**Field Descriptions:**

- `tokens` - Array of token IDs, or of `{"id", "piece"}` objects when `with_pieces` is `true`
<!-- END GENERATED: llamacpp.tokenize -->
