# Lemonade Server Spec

The `lemonade` SDK provides a standards-compliant server process that provides a REST API to enable communication with other applications.

Lemonade Server currently supports two backends:

| Backend                                                                 | Model Format | Description                                                                                                                |
|----------------------------------------------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------------|
| [ONNX Runtime GenAI (OGA)](https://github.com/microsoft/onnxruntime-genai) | `.ONNX`      | Lemonade's built-in server, recommended for standard use on AMD platforms.                                                |
| [Llama.cpp](https://github.com/ggml-org/llama.cpp) _(Experimental)_    | `.GGUF`      | Uses llama.cpp's Vulkan-powered llama-server backend. More details [here](#experimental-gguf-support).                    |


## OGA Endpoints Overview

Right now, the [key endpoints of the OpenAI API](#openai-compatible-endpoints) are available.

We are also actively investigating and developing [additional endpoints](#additional-endpoints) that will improve the experience of local applications.

### OpenAI-Compatible Endpoints
- POST `/api/v1/chat/completions` - Chat Completions (messages -> completion)
- POST `/api/v1/completions` - Text Completions (prompt -> completion)
- POST `api/v1/responses` - Chat Completions (prompt|messages -> event)
- GET `/api/v1/models` - List models available locally

### Additional Endpoints

> 🚧 These additional endpoints are a preview that is under active development. The API specification is subject to change.

These additional endpoints were inspired by the [LM Studio REST API](https://lmstudio.ai/docs/app/api/endpoints/openai), [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md), and [OpenAI API](https://platform.openai.com/docs/api-reference/introduction).

They focus on enabling client applications by extending existing cloud-focused APIs (e.g., OpenAI) to also include the ability to load and unload models before completion requests are made. These extensions allow for a greater degree of UI/UX responsiveness in native applications by allowing applications to:

- Pre-load models at UI-loading-time, as opposed to completion-request time.
- Load models from the local system that were downloaded by other applications (i.e., a common system-wide models cache).
- Unload models to save memory space.

The additional endpoints under development are:

- POST `/api/v1/pull` - Install a model
- POST `/api/v1/load` - Load a model
- POST `/api/v1/unload` - Unload a model
- POST `/api/v1/params` - Set generation parameters
- GET `/api/v1/health` - Check server health
- GET `/api/v1/stats` - Performance statistics from the last request

> 🚧 We are in the process of developing this interface. Let us know what's important to you on Github or by email (lemonade at amd dot com).

## Start the REST API Server

> **NOTE:** This server is intended for use on local systems only. Do not expose the server port to the open internet.

### Windows Installer

See the [Lemonade Server getting started instructions](./README.md). 

### Python Environment

If you have Lemonade [installed in a Python environment](https://lemonade-server.ai/install_options.html), simply activate it and run the following command to start the server:

```bash
lemonade-server-dev serve
```

## OpenAI-Compatible Endpoints


### `POST /api/v1/chat/completions` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

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
| `tools`       | No | A list of tools the model may call. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_completion_tokens`. This value is now deprecated by OpenAI in favor of `max_completion_tokens` | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_completion_tokens` | No | An upper bound for the number of tokens that can be generated for a completion. Mutually exclusive with `max_tokens`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

> Note: The value for `model` is either a [Lemonade Server model name](./server_models.md), or a checkpoint that has been pre-loaded using the [load endpoint](#get-apiv1load).

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:8000/api/v1/chat/completions" `
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
=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/chat/completions \
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

=== "Non-streaming responses"

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
=== "Streaming responses"
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


### `POST /api/v1/completions` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

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

> Note: The value for `model` is either a [Lemonade Server model name](./server_models.md), or a checkpoint that has been pre-loaded using the [load endpoint](#get-apiv1load).

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:8000/api/v1/completions" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "prompt": "What is the population of Paris?",
        "stream": false
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/completions \
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



### `POST /api/v1/responses` <sub>![Status](https://img.shields.io/badge/status-partially_available-green)</sub>

Responses API. You provide an input and receive a response. This API will also load the model if it is not already loaded.

#### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `input` | Yes | A list of dictionaries or a string input for the model to respond to. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `model` | Yes | The model to use for the response. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `max_output_tokens` | No | The maximum number of output tokens to generate. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `temperature` | No | What sampling temperature to use. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `stream` | No | If true, tokens will be sent as they are generated. If false, the response will be sent as a single message once complete. Defaults to false. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

> Note: The value for `model` is either a [Lemonade Server model name](./server_models.md), or a checkpoint that has been pre-loaded using the [load endpoint](#get-apiv1load).

#### Streaming Events

The Responses API uses semantic events for streaming. Each event is typed with a predefined schema, so you can listen for events you care about. Our initial implementation only offers support to:

- `response.created`
- `response.output_text.delta`
- `response.completed`

For a full list of event types, see the [API reference for streaming](https://platform.openai.com/docs/api-reference/responses-streaming).

#### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest -Uri "http://localhost:8000/api/v1/responses" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
        "model": "Llama-3.2-1B-Instruct-Hybrid",
        "input": "What is the population of Paris?",
        "stream": false
      }'
    ```

=== "Bash"

    ```bash
    curl -X POST http://localhost:8000/api/v1/responses \
      -H "Content-Type: application/json" \
      -d '{
            "model": "Llama-3.2-1B-Instruct-Hybrid",
            "input": "What is the population of Paris?",
            "stream": false
          }'
    ```


#### Response format

=== "Non-streaming responses"

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

=== "Streaming Responses"
    For streaming responses, the API returns a series of events. Refer to [OpenAI streaming guide](https://platform.openai.com/docs/guides/streaming-responses?api-mode=responses) for details.




### `GET /api/v1/models` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Returns a list of key models available on the server in an OpenAI-compatible format. We also expanded each model object with the `checkpoint` and `recipe` fields, which may be used to load a model using the `load` endpoint.

This [list](./server_models.md) is curated based on what works best for Ryzen AI Hybrid. Only models available locally are shown.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v1/models
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

### `GET /api/v1/pull` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Register and install models for use with Lemonade Server.

#### Parameters

The Lemonade Server built-in model registry has a collection of model names that can be pulled and loaded. The `pull` endpoint can install any registered model, and it can also register-then-install any model available on Hugging Face.

**Install a Model that is Already Registered**

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](./server_models.md) to install. |

Example request:

```bash
curl http://localhost:8000/api/v1/pull \
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

**Register and Install a Model**

Registration will place an entry for that model in the `user_models.json` file, which is located in the user's Lemonade cache (default: `~/.cache/lemonade`). Then, the model will be installed. Once the model is registered and installed, it will show up in the `models` endpoint alongside the built-in models and can be loaded.

The `recipe` field defines which software framework and device will be used to load and run the model. For more information on OGA and Hugging Face recipes, see the [Lemonade API README](../lemonade_api.md). For information on GGUF recipes, see [llamacpp](#experimental-gguf-support).

> Note: the `model_name` for registering a new model must use the `user` namespace, to prevent collisions with built-in models. For example, `user.Phi-4-Mini-GGUF`.

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | Namespaced [Lemonade Server model name](./server_models.md) to register and install. |
| `checkpoint` | Yes | HuggingFace checkpoint to install. |
| `recipe` | Yes | Lemonade API recipe to load the model with. |
| `reasoning` | No | Whether the model is a reasoning model, like DeepSeek (default: false). |
| `mmproj` | No | Multimodal Projector (mmproj) file to use for vision models. |

Example request:

```bash
curl http://localhost:8000/api/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "user.Phi-4-Mini-GGUF",
    "checkpoint": "unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M",
    "recipe": "llamacpp"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Installed model: user.Phi-4-Mini-GGUF"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `POST /api/v1/delete` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Delete a model by removing it from local storage. If the model is currently loaded, it will be unloaded first.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](./server_models.md) to delete. |

Example request:

```bash
curl http://localhost:8000/api/v1/delete \
  -H "Content-Type: application/json" \
  -d '{
    "model_name": "Qwen2.5-0.5B-Instruct-CPU"
  }'
```

Response format:

```json
{
  "status":"success",
  "message":"Deleted model: Qwen2.5-0.5B-Instruct-CPU"
}
```

In case of an error, the status will be `error` and the message will contain the error message.

### `GET /api/v1/load` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Explicitly load a registered model into memory. This is useful to ensure that the model is loaded before you make a request. Installs the model if necessary.

#### Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `model_name` | Yes | [Lemonade Server model name](./server_models.md) to load. |

Example request:

```bash
curl http://localhost:8000/api/v1/load \
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

### `POST /api/v1/unload` <sub>![Status](https://img.shields.io/badge/status-partially_available-red)</sub>

Explicitly unload a model from memory. This is useful to free up memory while still leaving the server process running (which takes minimal resources but a few seconds to start).

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v1/unload
```

#### Response format

```json
{
  "status": "success",
  "message": "Model unloaded successfully"
}
```
In case of an error, the status will be `error` and the message will contain the error message.

### `POST /api/v1/params` <sub>![Status](https://img.shields.io/badge/status-in_development-yellow)</sub>

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
curl http://localhost:8000/api/v1/params \
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

### `GET /api/v1/health` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Check the health of the server. This endpoint will also return the currently loaded model.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v1/health
```

#### Response format

```json
{
  "status": "ok",
  "checkpoint_loaded": "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
  "model_loaded": "Llama-3.2-1B-Instruct-Hybrid",
}
```
### `GET /api/v1/stats` <sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>

Performance statistics from the last request.

#### Parameters

This endpoint does not take any parameters.

#### Example request

```bash
curl http://localhost:8000/api/v1/stats
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
lemonade-server serve --log-level [level]
```

Where `[level]` can be one of:

- **critical**: Only critical errors that prevent server operation.
- **error**: Error conditions that might allow continued operation.
- **warning**: Warning conditions that should be addressed.
- **info**: (Default) General informational messages about server operation.
- **debug**: Detailed diagnostic information for troubleshooting, including metrics such as input/output token counts, Time To First Token (TTFT), and Tokens Per Second (TPS).
- **trace**: Very detailed tracing information, including everything from debug level plus all input prompts.

# Experimental GGUF Support

The OGA models (`*-CPU`, `*-Hybrid`) available in Lemonade Server use Lemonade's built-in server implementation. However, Lemonade SDK v7.0.1 introduced experimental support for [llama.cpp's](https://github.com/ggml-org/llama.cpp) Vulkan `llama-server` as an alternative backend for CPU and GPU.

The `llama-server` backend works with Lemonade's suggested `*-GGUF` models, as well as any .gguf model from Hugging Face. Windows and Ubuntu Linux are supported. Details:
- Lemonade Server wraps `llama-server` with support for the `lemonade-server` CLI, client web app, and endpoints (e.g., `models`, `pull`, `load`, etc.).
  - The `chat/completions` endpoint is the only completions/responses endpoint supported. 
  - Non-chat `completions`, and `responses` are not supported at this time.
- A single Lemonade Server process can seamlessly switch between OGA and GGUF models.
  - Lemonade Server will attempt to load models onto GPU with Vulkan first, and if that doesn't work it will fall back to CPU.
  - From the end-user's perspective, OGA vs. GGUF should be completely transparent: they wont be aware of whether the built-in server or `llama-server` is serving their model.

## Installing GGUF Models

To install an arbitrary GGUF from Hugging Face, open the Lemonade web app by navigating to http://localhost:8000 in your web browser and click the Model Management tab.

## Platform Support Matrix

| Platform | Vulkan GPU | x64 CPU      |
|----------|------------|--------------|
| Windows  | ✅         | ✅           |
| Ubuntu   | ✅         | ✅           |
| Other Linux | ⚠️*     | ⚠️*          |

*Other Linux distributions may work but are not officially supported.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
