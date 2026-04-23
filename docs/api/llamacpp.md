# llama.cpp-Specific API

This page documents Lemonade's llama.cpp-specific compatibility surface.

## Summary

| Method | Endpoint | Description | Modality |
|--------|----------|-------------|----------|
| `POST` | [`/v1/reranking`](#post-apiv1reranking) | Reranking | query + documents -> relevance-scored documents |

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
      }'
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
