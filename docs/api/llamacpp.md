# llama.cpp-Specific API

This page documents Lemonade's llama.cpp-specific compatibility surface.

## Summary

| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/reranking`](#post-v1reranking) | Reranking | query + documents -> relevance-scored documents |
| `GET` | [`/v1/slots`](#get-v1slots) | Returns the current slots processing state | slots state |
| `POST` | [`/v1/slots/{id}?action=save`](#post-v1slotsidactionsave) | Save the prompt cache of the specified slot to a file | prompt cache |
| `POST` | [`/v1/slots/{id}?action=restore`](#post-v1slotsidactionrestore) | Restore the prompt cache of the specified slot from a file | prompt cache |
| `POST` | [`/v1/slots/{id}?action=erase`](#post-v1slotsidactionerase) | Erase the prompt cache of the specified slot | prompt cache |

## `POST /v1/reranking`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Reranking API for llama.cpp-compatible reranker models. You provide a query and a list of documents, and receive relevance scores for each document. Lemonade will load the requested model automatically if it is not already loaded.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/v1/rerank` endpoint.

> **Note:** This endpoint is only available for reranker-specific models using the `llamacpp` recipe, such as `bge-reranker-v2-m3-GGUF`.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` | Yes | The search query text. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `documents` | Yes | Array of document strings to score against the query. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The reranking model to use. If not already loaded, Lemonade loads it before forwarding the request. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/reranking" `
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
      }' -UseBasicParsing
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:13305/v1/reranking \
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

```json
{
  "model": "bge-reranker-v2-m3-GGUF",
  "object": "list",
  "results": [
    {
      "index": 0,
      "relevance_score": 8.60673713684082
    },
    {
      "index": 1,
      "relevance_score": -5.3886260986328125
    },
    {
      "index": 2,
      "relevance_score": -3.555561065673828
    }
  ],
  "usage": {
    "prompt_tokens": 51,
    "total_tokens": 51
  }
}
```

**Field Descriptions:**

- `model` - Model identifier used for reranking
- `object` - Type of response object, always `"list"`
- `results` - Array of all input documents with relevance scores
  - `index` - Original index of the document in the input array
  - `relevance_score` - Relevance score assigned by the model; higher means more relevant
- `usage` - Token usage statistics
  - `prompt_tokens` - Number of tokens in the input
  - `total_tokens` - Total tokens processed

> **Note:** Results are returned in input order. To rank documents by relevance, sort `results` by `relevance_score` in descending order on the client side.

## `GET /v1/slots`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the current state of all processing slots in the llama.cpp server. Slots are parallel processing contexts that can handle multiple requests concurrently.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/slots` endpoint.

> **Note:** This endpoint is only available when a llama.cpp model is loaded.

> **Note:** This endpoint supports all four path prefixes: `/api/v0/slots`, `/api/v1/slots`, `/v0/slots`, and `/v1/slots`.

### Parameters

This endpoint accepts no parameters.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots" `
      -Method GET -UseBasicParsing
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/slots
    ```

### Response format

```json
[
  {
    "id": 0,
    "state": "idle",
    "next_token": {
      "has_next_token": false,
      "n_remain": 0,
      "n_decoded": 0
    },
    "task_id": -1,
    "cache_tokens": 1024
  },
  {
    "id": 1,
    "state": "processing",
    "next_token": {
      "has_next_token": true,
      "n_remain": 42,
      "n_decoded": 15
    },
    "task_id": 123,
    "cache_tokens": 512
  }
]
```

**Field Descriptions:**

- `id` - Unique identifier for the slot
- `state` - Current processing state ("idle", "processing", etc.)
- `next_token` - Information about token generation state
  - `has_next_token` - Whether more tokens are expected
  - `n_remain` - Number of tokens remaining to generate
  - `n_decoded` - Number of tokens already decoded
- `task_id` - Identifier of the current task being processed (-1 if idle)
- `cache_tokens` - Number of cached tokens in the slot's prompt cache

## `POST /v1/slots/{id}?action=save`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Save the prompt cache of a specific slot to a file. This allows you to persist the current context state for later restoration.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/slots/{id}?action=save` endpoint.

> **Note:** The llama.cpp server must be started with the `--slot-save-path` argument for save operations to work. See [Server Configuration](../server/configuration.md) for details on configuring backend arguments.
>
> Example configuration:
> ```bash
> lemonade config set llamacpp.args="--slot-save-path /path/to/slot/saves"
> ```

> **Note:** This endpoint supports all four path prefixes: `/api/v0/slots/{id}`, `/api/v1/slots/{id}`, `/v0/slots/{id}`, and `/v1/slots/{id}`.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` | Yes | The slot ID to save (path parameter). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `filename` | Yes | The filename where the slot cache should be saved (JSON body). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=save" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}' -UseBasicParsing
    ```

=== "PowerShell (/api/v1)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/v1/slots/0?action=save" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}' -UseBasicParsing
    ```

=== "Bash"

    ```bash
    curl -X POST "http://localhost:13305/v1/slots/0?action=save" \
      -H "Content-Type: application/json" \
      -d '{"filename": "my_conversation_cache.bin"}'
    ```

### Response format

```json
{
  "id_slot": 0,
  "filename": "my_conversation_cache.bin",
  "n_saved": 1024
}
```

**Field Descriptions:**

- `id_slot` - The slot ID that was saved
- `filename` - The filename where the cache was saved
- `n_saved` - Number of tokens saved to the cache file

## `POST /v1/slots/{id}?action=restore`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Restore the prompt cache of a specific slot from a previously saved file. This allows you to resume a conversation or context from where you left off.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/slots/{id}?action=restore` endpoint.

> **Note:** The llama.cpp server must be started with the `--slot-save-path` argument for restore operations to work.

> **Note:** This endpoint supports all four path prefixes: `/api/v0/slots/{id}`, `/api/v1/slots/{id}`, `/v0/slots/{id}`, and `/v1/slots/{id}`.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` | Yes | The slot ID to restore to (path parameter). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `filename` | Yes | The filename from which to restore the slot cache (JSON body). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=restore" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}' -UseBasicParsing
    ```

=== "PowerShell (/api/v1)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/v1/slots/0?action=restore" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}' -UseBasicParsing
    ```

=== "Bash"

    ```bash
    curl -X POST "http://localhost:13305/v1/slots/0?action=restore" \
      -H "Content-Type: application/json" \
      -d '{"filename": "my_conversation_cache.bin"}'
    ```

### Response format

```json
{
  "id_slot": 0,
  "filename": "my_conversation_cache.bin",
  "n_restored": 1024
}
```

**Field Descriptions:**

- `id_slot` - The slot ID that was restored
- `filename` - The filename from which the cache was restored
- `n_restored` - Number of tokens restored from the cache file

## `POST /v1/slots/{id}?action=erase`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Erase (clear) the prompt cache of a specific slot. This removes all cached context from the slot, resetting it to an empty state.

> **Note:** This endpoint is part of Lemonade's llama.cpp compatibility layer. Internally, Lemonade forwards the request to llama.cpp's `/slots/{id}?action=erase` endpoint.

> **Note:** This endpoint supports all four path prefixes: `/api/v0/slots/{id}`, `/api/v1/slots/{id}`, `/v0/slots/{id}`, and `/v1/slots/{id}`.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` | Yes | The slot ID to erase (path parameter). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=erase" `
      -Method POST -UseBasicParsing
    ```

=== "PowerShell (/api/v1)"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/api/v1/slots/0?action=erase" `
      -Method POST -UseBasicParsing
    ```

=== "Bash"

    ```bash
    curl -X POST "http://localhost:13305/v1/slots/0?action=erase"
    ```

### Response format

```json
{
  "id_slot": 0
}
```

**Field Descriptions:**

- `id_slot` - The slot ID that was erased

> **Note:** If the server returns an error, it may indicate that the slot was not found or that the operation failed.
