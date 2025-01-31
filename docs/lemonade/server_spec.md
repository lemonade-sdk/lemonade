# Lemonade Server Spec (Preview)

> This is a preview release. The API specification is subject to change.

This spec was inspired by the [LM Studio REST API](https://lmstudio.ai/docs/api-reference/rest-api), [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), and [OpenAI API](https://platform.openai.com/docs/api-reference/introduction).

This spec focuses on enabling client applications by extending existing cloud-focused APIs (e.g., OpenAI) to also include the ability to load and unload models before completion requests are made. These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:
- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Load models from the local system that were downloaded by other applications (i.e., a common system-wide models cache). 
- Unload models to save memory space.

## API Endpoints

- POST `/api/v0/completions` - Text Completions (prompt -> completion)
- POST `/api/v0/load` - Load a model
- POST `/api/v0/unload` - Unload a model
- POST `/api/v0/params` - Set generation parameters
- GET `/api/v0/health` - Check server health
- GET `/api/v0/stats` - Performance statistics from the last request
- GET `/api/v0/models` - List available models

> 🚧 We are in the process of developing this interface. Let us know what's important to you on Github or by email (turnkeyml at amd dot com).

## Start the REST API server

First, install lemonade with your desired backend (e.g., `pip install lemonade[llm-oga-cpu]`). Then, run the following command to start the server:

```bash
lemonade server-preview
```

## Endpoints

### `POST /api/v0/completions` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Text Completions API. You provide a prompt and receive a streamed completion. This API will also load the model if it is not already loaded.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The prompt to use for the completion.  | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | No | The model to use for the completion. If not specified, the server will use the default loaded model.  | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |   
| All other params of `/api/v0/load`  | No | Detailed loading options as defined in the `/api/v0/load` endpoint. | <sub>![Status](https://img.shields.io/badge/WIP-yellow)</sub> |
| All other params of `/api/v0/params` | No | Detailed generation options as defined in the `/api/v0/params` endpoint. | <sub>![Status](https://img.shields.io/badge/WIP-yellow)</sub> |

### Example request

```bash
curl http://localhost:1234/api/v0/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "<HUGGINGFACE_CHECKPOINT>",
    "prompt": "the meaning of life is",
  }'
```

### Response format

```json
{
  "text": " to find your purpose, and once you have",
}
```

### `GET /api/v0/load` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly load a model. This is useful to ensure that the model is loaded before you make a request.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model` | Yes | HuggingFace checkpoint to load. |
| `device` | No | Device to load the model on. Defaults to `hybrid`. |
| `dtype` | No | Data type to load the model on. Defaults to `int4`. |
| `cache_dir` | No | Parent directory where models are stored. Defaults to `~/.cache/lemonade`. |

### Example request

```bash
curl http://localhost:1234/api/v0/load \
  -H "Content-Type: application/json" \
  -d '{
    "model": "<HUGGINGFACE_CHECKPOINT>",
    "cache_dir": "/Users/your_username/models"
  }'
```

### Response format

```json
{
  "status": "success",
  "message": "Model loaded successfully"
}
```
In case of an error, the status will be `error` and the message will contain the error message.


### `POST /api/v0/unload` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly unload a model. This is useful to free up memory and disk space while still leaving the server runnning (which takes minimal resources but a few seconds to start).

### Parameters

This endpoint does not take any parameters.

### Example request

```bash
curl http://localhost:1234/api/v0/unload
```

### Response format

```json
{
  "status": "success",
  "message": "Model unloaded successfully"
}
```
In case of an error, the status will be `error` and the message will contain the error message.

### `POST /api/v0/params` <sub>![Status](https://img.shields.io/badge/status-in_development-yellow)</sub>
Set the generation parameters for text completion. These parameters will persist across requests until changed.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `temperature` | No | Controls randomness in the output. Higher values (e.g. 0.8) make the output more random, lower values (e.g. 0.2) make it more focused and deterministic. Defaults to 0.7. |
| `top_p` | No | Controls diversity via nucleus sampling. Keeps the cumulative probability of tokens above this value. Defaults to 0.95. |
| `top_k` | No | Controls diversity by limiting to the k most likely next tokens. Defaults to 50. |
| `min_length` | No | The minimum length of the generated text in tokens. Defaults to 0. |
| `max_length` | No | The maximum length of the generated text in tokens. Defaults to 2048. |
| `do_sample` | No | Whether to use sampling (true) or greedy decoding (false). Defaults to true. |

### Example request

```bash
curl http://localhost:1234/api/v0/params \
  -H "Content-Type: application/json" \
  -d '{
    "temperature": 0.8,
    "top_p": 0.95,
    "max_length": 1000
  }'
```

### Response format

```json
{
  "status": "success",
  "message": "Generation parameters set successfully",
  "params": {
    "temperature": 0.8,
    "top_p": 0.95,
    "top_k": 40,
    "min_length": 0,
    "max_length": 1000,
    "do_sample": true
  }
}
```
In case of an error, the status will be `error` and the message will contain the error message.

### `GET /api/v0/health` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Check the health of the server. This endpoint will also return the currently loaded model.

### Parameters

This endpoint does not take any parameters.

### Example request

```bash
curl http://localhost:1234/api/v0/health
```

### Response format

```json
{
  "status": "ok",
  "model_loaded": "<HUGGINGFACE_CHECKPOINT>"
}
```
### `GET /api/v0/stats` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Performance statistics from the last request.

### Parameters

This endpoint does not take any parameters.

### Example request

```bash
curl http://localhost:1234/api/v0/stats
```

### Response format

```json
{
  "time_to_first_token": 2.14,
  "tokens_per_second": 33.33,
  "input_tokens": 128,
  "output_tokens": 5,
  "decode_token_times": [0.01, 0.02, 0.03, 0.04, 0.05]
}
```

### `GET /api/v0/models` <sub>![Status](https://img.shields.io/badge/status-in_development-yellow)</sub>

List all available models.

### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `cache_dir` | No | Parent directory where models are stored. Defaults to `~/.cache/lemonade`. |

### Example request

```bash
curl http://localhost:1234/api/v0/models \
  -H "Content-Type: application/json" \
  -d '{
    "cache_dir": "/Users/your_username/models"
  }'
```

### Response format

```json
{
  "data": [
    {
      "checkpoint": "<HUGGINGFACE_CHECKPOINT>",
      "device": "cpu",
      "dtype": "bfloat16",
    },
    {
      "checkpoint": "<ANOTHER_HUGGINGFACE_CHECKPOINT>",
      "device": "cpu",
      "dtype": "bfloat16",
    }
  ]
}
```
