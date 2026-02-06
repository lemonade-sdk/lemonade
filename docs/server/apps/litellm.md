# Using Lemonade with LiteLLM

[LiteLLM](https://docs.litellm.ai/) is a powerful gateway that allows you to manage multiple LLM providers (OpenAI, Anthropic, Azure, etc.) using a unified format. By connecting Lemonade to LiteLLM, you can expose your local models to any application that supports LiteLLM, while gaining features like logging, caching, and budget management.

This guide covers the general configuration for using Lemonade as an OpenAI-compatible provider within LiteLLM.

## Prerequisites

- **Lemonade Server** installed and running on port 8000 (default).
- **LiteLLM** installed: `pip install litellm` (or `pip install 'litellm[proxy]'` for the proxy server).

## Configuration (`config.yaml`)

To use Lemonade with the LiteLLM Proxy, you need to define it as a model provider in your `config.yaml`.

Since Lemonade is OpenAI-compatible, we use the `openai/` prefix in the `model` parameter to tell LiteLLM to use the OpenAI protocol, but we override the `api_base` to point to your local machine.

```yaml
model_list:
  # Option 1: Direct mapping
  # This maps the name "gpt-4" (what clients ask for) to your local Lemonade model
  - model_name: gpt-4
    litellm_params:
      model: openai/Qwen2.5-Coder-7B-Instruct-GGUF  # The actual model loaded in Lemonade
      api_base: http://localhost:8000/v1
      api_key: lemonade

  # Option 2: Passthrough with prefix
  # This allows you to use specific Lemonade models by name
  - model_name: lemonade-qwen
    litellm_params:
      model: openai/Qwen2.5-Coder-7B-Instruct-GGUF
      api_base: http://localhost:8000/v1
      api_key: lemonade
```

> **Note on Model Names:** 
> Lemonade's server automatically handles model names. If you configure LiteLLM to send `model: lemonade/my-model`, Lemonade will strip the prefix and load `my-model`.

## Running the Proxy

Once your configuration is saved, start the proxy:

```bash
litellm --config config.yaml
```

The proxy will run on `http://0.0.0.0:4000` by default.

## Usage Examples

### 1. Using the Python SDK

You can use the `litellm` Python library to call your local Lemonade models directly, without running the proxy server.

```python
from litellm import completion

response = completion(
    model="openai/Qwen2.5-Coder-7B-Instruct-GGUF",
    api_base="http://localhost:8000/v1",
    api_key="lemonade",
    messages=[{ "content": "Hello, how are you?","role": "user"}]
)

print(response)
```

### 2. Using `curl` via the Proxy

If the proxy is running on port 4000:

```bash
curl http://0.0.0.0:4000/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer sk-1234" \
  -d '{
    "model": "lemonade-qwen",
    "messages": [
      {"role": "user", "content": "Write a Python function to reverse a string."}
    ]
  }'
```

### 3. Advanced: Load Balancing

You can use LiteLLM to load balance between your local Lemonade server and cloud providers (like OpenAI or Anthropic). This is useful for "bursting" to the cloud when your local hardware is busy.

```yaml
router_settings:
  routing_strategy: latency  # Route to the fastest responder
  
model_list:
  - model_name: my-coding-assistant
    litellm_params:
      model: openai/Qwen2.5-Coder-7B-Instruct-GGUF
      api_base: http://localhost:8000/v1
      api_key: lemonade
      tpm: 1000  # Set limits for your local hardware

  - model_name: my-coding-assistant
    litellm_params:
      model: openai/gpt-4o
      api_key: os.environ/OPENAI_API_KEY
```

## Supported Parameters

Lemonade supports the standard OpenAI parameters passed through LiteLLM, including:

- `temperature`
- `top_p`
- `max_tokens` (or `max_completion_tokens`)
- `stop`
- `stream`
- `tools` (Function calling)
- `response_format` (JSON mode)
- `logit_bias`
- `presence_penalty`
- `frequency_penalty`

## Troubleshooting

**"Model not found" errors:**
Ensure that the `model` specified in `litellm_params` matches exactly what `lemonade-server list` returns, OR that you are using the `openai/` prefix so LiteLLM knows which protocol to use.

**Context Window Errors:**
If you see errors about context length, ensure your `config.yaml` does not enforce a limit lower than what Lemonade supports. You can add `model_info` to your config to define context windows explicitly for LiteLLM.
