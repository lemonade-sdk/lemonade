# Lemonade Server Spec

The `lemonade` SDK provides a standards-compliant server process that provides a REST API to enable communication with other applications. Right now, the [key endpoints of the OpenAI API](#openai-compatible-endpoints) are available. Our plan is to add more OpenAI endpoints, as well as Ollama-compatible endpoints, in the near future.

We are also actively investigating and developing [additional endpoints](#additional-endpoints) that will improve the experience of local applications.

## Endpoints Overview

### OpenAI-Compatible Endpoints
- POST `/api/v0/chat/completions` - Chat Completions (messages -> completion)
- POST `/api/v0/completions` - Text Completions (prompt -> completion)
- POST `api/v0/responses` - Chat Completions (prompt|messages -> event)
- GET `/api/v0/models` - List models available locally

### Additional Endpoints

> 🚧 These additional endpoints are a preview that is under active development. The API specification is subject to change.

These additional endpoints were inspired by the [LM Studio REST API](https://lmstudio.ai/docs/api-reference/rest-api), [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), and [OpenAI API](https://platform.openai.com/docs/api-reference/introduction).

They focus on enabling client applications by extending existing cloud-focused APIs (e.g., OpenAI) to also include the ability to load and unload models before completion requests are made. These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:
- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Load models from the local system that were downloaded by other applications (i.e., a common system-wide models cache). 
- Unload models to save memory space.

The additional endpoints under development are:
- POST `/api/v0/pull` - Install a model
- POST `/api/v0/load` - Load a model
- POST `/api/v0/unload` - Unload a model
- POST `/api/v0/params` - Set generation parameters
- GET `/api/v0/health` - Check server health
- GET `/api/v0/stats` - Performance statistics from the last request

> 🚧 We are in the process of developing this interface. Let us know what's important to you on Github or by email (turnkeyml at amd dot com).

## Start the REST API Server

> **NOTE:** This server is intended for use on local systems only. Do not expose the server port to the open internet.

### Windows Installer

See the [Lemonade_Server_Installer.exe instructions](lemonade_server_exe.md) to get started. 

### Python Environment

If you have Lemonade [installed in a Python environment](README.md#install), simply activate it and run the following command to start the server:

```bash
lemonade serve
```

## OpenAI-Compatible Endpoints


### `POST /api/v0/chat/completions` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Chat Completions API. You provide a list of messages and receive a completion. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `messages` | Yes | Array of messages in the conversation. Each message should have a `role` ("user" or "assistant") and `content` (the message text). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the completion. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Up to 4 sequences where the API will stop generating further tokens. The returned text will not contain the stop sequence. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Include log probabilities of the output tokens. If true, returns the log probability of each output token. Defaults to false. | <sub>![Status](https://img.shields.io/badge/not_available-red)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `tools`       | No | A list of tools the model may call. Only available when `stream` is set to `False`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_completion_tokens`. This value is now deprecated by OpenAI in favor of `max_completion_tokens` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

> Note: The value for `model` is either a [Lemonade Server model name](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md), or a checkpoint that has been pre-loaded using the [load endpoint](#get-apiv0load-status).

#### Example request

PowerShell:

```powershell
Invoke-WebRequest `
  -Uri "http://localhost:8000/api/v0/chat/completions" `
  -Method POST `
  -Headers @{ "Content-Type" = "application/json" } `
  -Body '{
    "model": "Llama-3.2-1B-Instruct-Hybrid",
    "messages": [
      {
        "role": "user",
        "content": "What is the population of Paris?"
      }
    ],
    "stream": false
  }'
```

Bash:

```bash
curl -X POST http://localhost:8000/api/v0/chat/completions \
  -H "Content-Type: application/json" \
  -d '{ 
        "model": "Llama-3.2-1B-Instruct-Hybrid", 
        "messages": [ 
          {"role": "user", "content": "What is the population of Paris?"} 
        ], 
        "stream": false
      }'
```

#### Response format

For non-streaming responses:

```json
{
  "id": "0",
  "object": "chat.completion",
  "created": 1742927481,
  "model": "Llama-3.2-1B-Instruct-Hybrid",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "Paris has a population of approximately 2.2 million people in the city proper."
    },
    "finish_reason": "stop"
  }]
}
```

For streaming responses, the API returns a stream of server-sent events (however, Open AI recommends using their streaming libraries for parsing streaming responses):
```json
{
  "id": "0",
  "object": "chat.completion.chunk",
  "created": 1742927481,
  "model": "Llama-3.2-1B-Instruct-Hybrid",
  "choices": [{
    "index": 0,
    "delta": {
      "role": "assistant",
      "content": "Paris"
    }
  }]
}
```


### `POST /api/v0/completions` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Text Completions API. You provide a prompt and receive a completion. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `prompt` | Yes | The prompt to use for the completion.  | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the completion.  | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stop` | No | Up to 4 sequences where the API will stop generating further tokens. The returned text will not contain the stop sequence. Can be a string or an array of strings. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `echo` | No | Echo back the prompt in addition to the completion. Available on non-streaming mode. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `logprobs` | No | Include log probabilities of the output tokens. If true, returns the log probability of each output token. Defaults to false. Only available when `stream=False`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion, including input tokens. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

> Note: The value for `model` is either a [Lemonade Server model name](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md), or a checkpoint that has been pre-loaded using the [load endpoint](#get-apiv0load-status).

#### Example request

PowerShell:

```powershell
Invoke-WebRequest -Uri "http://localhost:8000/api/v0/completions" `
  -Method POST `
  -Headers @{ "Content-Type" = "application/json" } `
  -Body '{
    "model": "Llama-3.2-1B-Instruct-Hybrid",
    "prompt": "What is the population of Paris?",
    "stream": false
  }'
```

Bash:

```bash
curl -X POST http://localhost:8000/api/v0/completions \
  -H "Content-Type: application/json" \
  -d '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "prompt": "What is the population of Paris?",
        "stream": false
      }'
```

#### Response format

The following format is used for both streaming and non-streaming responses:
```json
{
  "id": "0",
  "object": "text_completion",
  "created": 1742927481,
  "model": "Llama-3.2-1B-Instruct-Hybrid",
  "choices": [{
    "index": 0,
    "text": "Paris has a population of approximately 2.2 million people in the city proper.",
    "finish_reason": "stop"
  }],
}
```



### `POST /api/v0/responses` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Responses API. You provide an input and receive a response. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | A list of dictionaries or a string input for the model to respond to. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the response. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_output_tokens` | No | The maximum number of output tokens to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

> Note: The value for `model` is either a [Lemonade Server model name](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md), or a checkpoint that has been pre-loaded using the [load endpoint](#get-apiv0load-status).

#### Streaming Events

The Responses API uses semantic events for streaming. Each event is typed with a predefined schema, so you can listen for events you care about. Our initial implementation only offers support to:
- `response.created`
- `response.output_text.delta`
- `response.completed`

For a full list of event types, see the [API reference for streaming](https://platform.openai.com/docs/api-reference/responses-streaming).

#### Example request

PowerShell:

```powershell
Invoke-WebRequest -Uri "http://localhost:8000/api/v0/responses" `
  -Method POST `
  -Headers @{ "Content-Type" = "application/json" } `
  -Body '{
    "model": "Llama-3.2-1B-Instruct-Hybrid",
    "input": "What is the population of Paris?",
    "stream": false
  }'
```

Bash:

```bash
curl -X POST http://localhost:8000/api/v0/responses \
  -H "Content-Type: application/json" \
  -d '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "input": "What is the population of Paris?",
        "stream": false
      }'
```


#### Response format

For non-streaming responses:

```json
{
  "id": "0",
  "created_at": 1746225832.0,
  "model": "Llama-3.2-1B-Instruct-Hybrid",
  "object": "response",
  "output": [{
    "id": "0",
    "content": [{
      "annotations": [],
      "text": "Paris has a population of approximately 2.2 million people in the city proper."
    }]
  }]
}
```

For streaming responses, the API returns a series of events. Refer to [OpenAI streaming guide](https://platform.openai.com/docs/guides/streaming-responses?api-mode=responses) for details.




### `GET /api/v0/models` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns a list of key models available on the server in an OpenAI-compatible format. We also expanded each model object with the `checkpoint` and `recipe` fields, which may be used to load a model using the `load` endpoint.

This [list](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md) is curated based on what works best for Ryzen AI Hybrid. Only models available locally are shown.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v0/models
```

#### Response format

```json
{
  "object": "list",
  "data": [
    {
      "id": "Qwen2.5-0.5B-Instruct-CPU",
      "created": 1744173590,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx",
      "recipe": "oga-cpu"
    },
    {
      "id": "Llama-3.2-1B-Instruct-Hybrid",
      "created": 1744173590,
      "object": "model",
      "owned_by": "lemonade",
      "checkpoint": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
      "recipe": "oga-hybrid"
    },
  ]
}
```

## Additional Endpoints

### `GET /api/v0/pull` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Install a model by downloading it and registering it with Lemonade Server.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md) to load. |

Example request:

```bash
curl http://localhost:8000/api/v0/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Installed model: Qwen2.5-0.5B-Instruct-CPU"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `GET /api/v0/load` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly load a model into memory. This is useful to ensure that the model is loaded before you make a request. Installs the model if necessary.

#### Parameters

There are two distinct ways to load a model:
 - Load by Lemonade Server model name: uses the short names such as "Qwen2.5-0.5B-Instruct-CPU" found throughout Lemonade Server. The names are documented [here](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md).
 - Load by checkpoint and recipe: uses a Hugging Face checkpoint as the model source, and then a Lemonade API recipe that determines the framework/device backend to use (e.g., "oga-cpu"). For more information on Lemonade recipes, see the [Lemonade API ReadMe](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/lemonade_api.md).

The parameters for these two ways of loading are mutually exclusive. We intend load-by-name to be used in the general case, since that references a curated set of models in a concise way. Load-by-checkpoint can be used in the event that a user/developer wants to try a model that isn't in the curated list.

**Load by Lemonade Server Model Name (Recommended)**

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md) to load. |

Example request:

```bash
curl http://localhost:8000/api/v0/load \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Loaded model: Qwen2.5-0.5B-Instruct-CPU"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

**Load by Hugging Face Checkpoint and Lemonade Recipe**

> Note: load-by-checkpoint will download that checkpoint if it is not already available in your Hugging Face cache.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `checkpoint` | Yes | HuggingFace checkpoint to load. |
| `recipe` | Yes | Lemonade API recipe to load the model on. |
| `reasoning` | No | Whether the model is a reasoning model, like DeepSeek (default: false). |

Example request:

```bash
curl http://localhost:8000/api/v0/load \
  -H "Content-Type: application/json" \
  -d '{
    "checkpoint": "amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx",
    "recipe": "oga-cpu"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Loaded model: amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `POST /api/v0/unload` <sub>![Status](https://img.shields.io/badge/status-partially_available-red)</sub>

Explicitly unload a model from memory. This is useful to free up memory while still leaving the server process running (which takes minimal resources but a few seconds to start).

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v0/unload
```

#### Response format

```json
{
  "status": "success",
  "message": "Model unloaded successfully"
}
```
In case of an error, the status will be `error` and the message will contain the error message.

### `POST /api/v0/params` <sub>![Status](https://img.shields.io/badge/status-in_development-yellow)</sub>
Set the generation parameters for text completion. These parameters will persist across requests until changed.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `temperature` | No | Controls randomness in the output. Higher values (e.g. 0.8) make the output more random, lower values (e.g. 0.2) make it more focused and deterministic. Defaults to 0.7. |
| `top_p` | No | Controls diversity via nucleus sampling. Keeps the cumulative probability of tokens above this value. Defaults to 0.95. |
| `top_k` | No | Controls diversity by limiting to the k most likely next tokens. Defaults to 50. |
| `min_length` | No | The minimum length of the generated text in tokens. Defaults to 0. |
| `max_length` | No | The maximum length of the generated text in tokens. Defaults to 2048. |
| `do_sample` | No | Whether to use sampling (true) or greedy decoding (false). Defaults to true. |

#### Example request

```bash
curl http://localhost:8000/api/v0/params \
  -H "Content-Type: application/json" \
  -d '{
    "temperature": 0.8,
    "top_p": 0.95,
    "max_length": 1000
  }'
```

#### Response format

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

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v0/health
```

#### Response format

```json
{
  "status": "ok",
  "checkpoint_loaded": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
  "model_loaded": "Llama-3.2-1B-Instruct-Hybrid",
}
```
### `GET /api/v0/stats` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Performance statistics from the last request.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v0/stats
```

#### Response format

```json
{
  "time_to_first_token": 2.14,
  "tokens_per_second": 33.33,
  "input_tokens": 128,
  "output_tokens": 5,
  "decode_token_times": [0.01, 0.02, 0.03, 0.04, 0.05]
}
```

# Debugging

To help debug the Lemonade server, you can use the `--log-level` parameter to control the verbosity of logging information. The server supports multiple logging levels that provide increasing amounts of detail about server operations.

```
lemonade serve --log-level [level]
```

Where `[level]` can be one of:

- **critical**: Only critical errors that prevent server operation.
- **error**: Error conditions that might allow continued operation.
- **warning**: Warning conditions that should be addressed.
- **info**: (Default) General informational messages about server operation.
- **debug**: Detailed diagnostic information for troubleshooting, including metrics such as input/output token counts, Time To First Token (TTFT), and Tokens Per Second (TPS).
- **trace**: Very detailed tracing information, including everything from debug level plus all input prompts.
