# llama.cpp-Specific API

This page documents Lemonade's llama.cpp-specific compatibility surface. Lemonade forwards each of these requests to a llama.cpp server: rerank to the model it names, and slots and tokenize, which name no model, to the most recently used loaded model.

<!-- BEGIN GENERATED: llamacpp.summary -->
| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/rerank`](#post-v1rerank) | Score documents against a query | query + documents -> relevance-scored documents |
| `GET` | [`/v1/slots`](#get-v1slots) | Read each slot's processing state | slots state |
| `POST` | [`/v1/slots/{id}?action=save`](#post-v1slotsid) | Save the prompt cache of the specified slot to a file | prompt cache |
| `POST` | [`/v1/slots/{id}?action=restore`](#post-v1slotsid) | Restore the prompt cache of the specified slot from a file | prompt cache |
| `POST` | [`/v1/slots/{id}?action=erase`](#post-v1slotsid) | Erase the prompt cache of the specified slot | prompt cache |
| `POST` | [`/v1/tokenize`](#post-v1tokenize) | Tokenize text | tokenization |
<!-- END GENERATED: llamacpp.summary -->

<!-- BEGIN GENERATED: llamacpp.rerank -->
## `POST /v1/rerank`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Scores each document against a query. Only llama.cpp reranker models, such as `bge-reranker-v2-m3-GGUF`, serve this endpoint.

Also served at `/v1/reranking` and `/v1/reranker`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `query` | Yes | The search query text. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `documents` | Yes | Documents to score against the query. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to run; loaded on first use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `ctx_size` | No | Lemonade extension: context size to load the model with, when this request loads it. Ignored when the model is already loaded. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{
  "model": "bge-reranker-v2-m3-GGUF",
  "object": "list",
  "results": [{"index": 0, "relevance_score": 8.634263038635254}, ... 2 more],
  "usage": {"prompt_tokens": 51, "total_tokens": 51}
}
```

### Response format

- `model` - The model that scored the documents.
- `object` - Always `"list"`.
- `results` - Every input document with its score, sorted by `relevance_score`, highest first.
  - `index` - The document's position in `documents`.
  - `relevance_score` - How relevant the model rates the document; higher is more relevant.
- `usage` - Tokens processed.
  - `prompt_tokens` - Tokens in the query and documents.
  - `total_tokens` - All tokens processed.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `model` is not a llama.cpp model |
| `404` | `model_not_supported` | The requested model is filtered out on this system, such as an NPU model without an NPU |
| `404` | `model_not_found` | The requested model is not in the registry |
| `409` | `router_residency_conflict` | Loading the requested model would evict a model that a `collection.router` model needs resident on the same NPU |
| `409` | `slots_pinned_error` | Every loaded model of the requested model's type is pinned, so none can be unloaded to make room |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
| `500` | `model_load_error` | Loading the requested model fails for another reason, such as a failed download or a backend that does not start |
<!-- END GENERATED: llamacpp.rerank -->

<!-- BEGIN GENERATED: llamacpp.slots -->
## `GET /v1/slots`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns the state of each processing slot in the llama.cpp server. Slots are the parallel contexts that serve concurrent requests.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

This endpoint does not take any parameters.

### Example

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:13305/v1/slots"
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/slots
    ```

**Response:** `200 OK`

```json
[
  {
    "id": 0,
    "next_token": [
      {"has_next_token": false, "has_new_line": false, "n_remain": -1, "n_decoded": 0}
    ],
    "id_task": 212,
    "n_ctx": 40960,
    "is_processing": false,
    "n_prompt_tokens": 224,
    "n_prompt_tokens_cache": 0,
    "n_prompt_tokens_processed": 0,
    "params": {
      "adaptive_decay": 0.8999999761581421,
      "adaptive_target": -1.0,
      "backend_sampling": false,
      "chat_format": "peg-native",
      "dry_allowed_length": 2,
      "dry_base": 1.75,
      "dry_multiplier": 0.0,
      "dry_penalty_last_n": 64,
      "dynatemp_exponent": 1.0,
      "dynatemp_range": 0.0,
      "frequency_penalty": 0.0,
      "generation_prompt": "<|im_start|>assistant\n",
      "ignore_eos": false,
      "lora": [],
      "max_tokens": -1,
      "min_keep": 0,
      "min_p": 0.0,
      "mirostat": 0,
      "mirostat_eta": 0.10000000149011612,
      "mirostat_tau": 5.0,
      "n_discard": 0,
      "n_keep": 0,
      "n_predict": -1,
      "n_probs": 0,
      "post_sampling_probs": false,
      "presence_penalty": 0.0,
      "reasoning_format": "deepseek",
      "reasoning_in_content": false,
      "repeat_last_n": 64,
      "repeat_penalty": 1.0,
      "samplers": [
        "penalties",
        "dry",
        "top_n_sigma",
        "top_k",
        "typ_p",
        "top_p",
        "min_p",
        "xtc",
        "temperature"
      ],
      "seed": 4294967295,
      "speculative.types": "none",
      "stream": false,
      "temperature": 0.0,
      "timings_per_token": false,
      "top_k": 20,
      "top_n_sigma": -1.0,
      "top_p": 0.8500000238418579,
      "typical_p": 1.0,
      "xtc_probability": 0.0,
      "xtc_threshold": 0.10000000149011612
    },
    "speculative": false
  }
]
```

### Response format

- `id` - The slot's ID.
- `next_token` - Generation state of the slot's current request.
  - `has_next_token` - Whether more tokens are expected
  - `has_new_line` - Whether the request's generated text contains a newline.
  - `n_remain` - Number of tokens remaining to generate
  - `n_decoded` - Number of tokens already decoded
- `id_task` - The task the slot is processing, or processed last when idle.
- `n_ctx` - Context size of the slot.
- `is_processing` - Whether the slot is serving a request.
- `n_prompt_tokens` - Tokens in the slot's current prompt.
- `n_prompt_tokens_cache` - Prompt tokens reused from the slot's cache instead of processed again.
- `n_prompt_tokens_processed` - Prompt tokens processed for the current request.
- `params` - Sampling parameters of the slot's last request.
- `speculative` - Whether the slot uses speculative decoding.

### Errors

| Status | When |
|--------|------|
| `400` | No model is loaded |
| `400` | The most recently used model is not a llama.cpp model |
| `404` | The model is unloaded while the request waits for it |
| `500` | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` | The request fails unexpectedly |
<!-- END GENERATED: llamacpp.slots -->

<!-- BEGIN GENERATED: llamacpp.slots_action -->
## `POST /v1/slots/{id}`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Saves a slot's prompt cache to a file, restores it from one, or erases it, so a conversation can resume where it left off.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `id` (path) | Yes | The slot to act on. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `filename` | No | File to save to or restore from, relative to `--slot-save-path`. Required for `save` and `restore`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `action` (query) | Yes | `save` writes the slot's prompt cache to `filename`, `restore` reads it back from `filename`, and `erase` clears the slot. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Every action needs llama.cpp started with `--slot-save-path`. Set it for every load with `lemonade config set llamacpp.args="--slot-save-path /path/to/slot/saves"` (see [Server Configuration](../guide/configuration/README.md)), or for one load with `llamacpp_args` on [`POST /v1/load`](./lemonade.md#post-v1load).

### Examples

#### Save

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

**Response:** `200 OK`

```json
{
  "id_slot": 0,
  "filename": "my_conversation_cache.bin",
  "n_saved": 46,
  "n_written": 5277100,
  "timings": {"save_ms": 3.777}
}
```

#### Restore

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/slots/0?action=restore" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{"filename": "my_conversation_cache.bin"}'
    ```

=== "Bash"

    ```bash
    curl "http://localhost:13305/v1/slots/0?action=restore" \
      -H "Content-Type: application/json" \
      -d '{"filename": "my_conversation_cache.bin"}'
    ```

**Response:** `200 OK`

```json
{
  "id_slot": 0,
  "filename": "my_conversation_cache.bin",
  "n_restored": 46,
  "n_read": 5277100,
  "timings": {"restore_ms": 0.879}
}
```

#### Erase

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

**Response:** `200 OK`

```json
{"id_slot": 0, "n_erased": 224}
```

### Response format

- `id_slot` - The slot acted on.
- `filename` - `save` and `restore`: the file written or read.
- `n_saved` - `save`: tokens written.
- `n_restored` - `restore`: tokens read.
- `n_written` - `save`: bytes written.
- `n_read` - `restore`: bytes read.
- `n_erased` - `erase`: tokens cleared.
- `timings` - `save` and `restore`: how long the file operation took.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | `id` is too large for a 32-bit integer |
| `400` |  | No model is loaded |
| `400` |  | The most recently used model is not a llama.cpp model |
| `404` |  | The model is unloaded while the request waits for it |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: llamacpp.slots_action -->

<!-- BEGIN GENERATED: llamacpp.tokenize -->
## `POST /v1/tokenize`
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Tokenizes text without using the model's context window. After a request to a reranker, for example, the reranker's tokenizer answers.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `content` | Yes | The text to tokenize. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `add_special` | No | Insert special tokens, such as `BOS`. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `parse_special` | No | Tokenize special tokens; `false` treats them as plain text. Defaults to `true`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `with_pieces` | No | Return each token's text piece with its ID. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Example

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

**Response:** `200 OK`

```json
{"tokens": [1986, 374, 264, 914, 311, 77651]}
```

### Response format

- `tokens` - Token IDs, or `{"id", "piece"}` objects when `with_pieces` is `true`.

### Errors

| Status | Code | When |
|--------|------|------|
| `400` | `invalid_json` | The body is not a JSON object |
| `400` | `missing_argument` | A required argument is missing |
| `400` | `invalid_argument` | An argument or a value nested in it has the wrong JSON type, or a value its schema does not list |
| `400` |  | No model is loaded |
| `400` |  | The most recently used model is not a llama.cpp model |
| `404` |  | The model is unloaded while the request waits for it |
| `500` |  | The backend reports an error; the backend's own status replaces this one when it sends one |
| `500` |  | The request fails unexpectedly |
<!-- END GENERATED: llamacpp.tokenize -->
