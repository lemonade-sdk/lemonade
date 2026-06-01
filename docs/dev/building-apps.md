# Building Apps with Lemonade

This guide is for developers building applications that talk to a Lemonade server over HTTP — web apps, desktop clients, agents, and background services. It covers the connection pattern, model discovery, inference, streaming, error handling, and security.

If you want to **embed** a Lemonade server inside your own installer or process, see the [Embeddable Lemonade](../embeddable/README.md) guide instead.

**Contents:**

- [Connection pattern](#connection-pattern)
- [Discover models](#discover-models)
- [Run inference](#run-inference)
- [Handle streaming](#handle-streaming)
- [Manage models at runtime](#manage-models-at-runtime)
- [Error handling reference](#error-handling-reference)
- [Authentication](#authentication)
- [Performance tips](#performance-tips)

---

## Connection pattern

Lemonade Server (`lemond`) exposes an HTTP API on `http://localhost:13305` by default. All inference and management endpoints are available under the `/v1/` prefix (OpenAI-compatible) and the `/api/v1/` prefix (same routes, legacy prefix).

The simplest possible check — confirm the server is up and get its version:

```bash
curl http://localhost:13305/v1/health
```

```json
{
  "status": "ok",
  "version": "9.3.3",
  "model_loaded": null,
  "all_models_loaded": [],
  "max_models": {"llm": 1, "embedding": 1, "image": 1, "transcription": 1, "tts": 1, "reranking": 1}
}
```

The `/live` endpoint is a lighter-weight liveness probe (no model inspection) suitable for high-frequency health checks:

```bash
curl http://localhost:13305/live
```

### Discover the server on the local network

If you don't know which machine is running the server, `lemond` broadcasts a UDP beacon on port 13305. The CLI command `lemonade scan` listens for these beacons. From your own code:

1. Open a UDP socket and listen on port 13305.
2. Each beacon is a JSON payload with `service`, `hostname`, and `url` fields.
3. Use `url` as your base URL.

For remote servers you control, set the host explicitly via `lemonade config set host=0.0.0.0` on the server and point your client at the server's IP.

---

## Discover models

### List available models

```python
import requests

BASE_URL = "http://localhost:13305"

models = requests.get(f"{BASE_URL}/v1/models").json()["data"]
```

Each model object includes:

| Field | Description |
|-------|-------------|
| `id` | Model name used in inference requests |
| `labels` | Array of capability labels (e.g. `["reasoning", "tool-calling"]`) |
| `downloaded` | `true` if the model files are present on disk |
| `recipe` | Backend recipe (e.g. `llamacpp`, `flm`, `sd-cpp`) |

By default, the listing hides collections and some internal entries. Pass `?show_all=true` to see everything:

```python
all_models = requests.get(f"{BASE_URL}/v1/models?show_all=true").json()["data"]
```

### Filter by capability

Use the `labels` array to find models for a specific task:

```python
llm_models    = [m for m in models if not any(l in m.get("labels", [])
                  for l in ("image", "transcription", "tts", "embeddings", "reranking"))]
image_models  = [m for m in models if "image"         in m.get("labels", [])]
tts_models    = [m for m in models if "tts"           in m.get("labels", [])]
asr_models    = [m for m in models if "transcription" in m.get("labels", [])]
embed_models  = [m for m in models if "embeddings"    in m.get("labels", [])]
vision_models = [m for m in models if "vision"        in m.get("labels", [])]
```

### Check whether a model is loaded

`GET /v1/health` returns `all_models_loaded` — the currently loaded models and their types. Cross-reference with your target model name:

```python
health = requests.get(f"{BASE_URL}/v1/health").json()
loaded_names = {m["model_name"] for m in health.get("all_models_loaded", [])}
is_loaded = "Qwen3-0.6B-GGUF" in loaded_names
```

---

## Run inference

All inference endpoints follow the OpenAI API shape. The server auto-loads the requested model on the first request if it is not already loaded — expect a startup delay of a few seconds the first time.

### Chat completion

```python
from openai import OpenAI

client = OpenAI(base_url="http://localhost:13305/v1", api_key="lemonade")

response = client.chat.completions.create(
    model="Qwen3-0.6B-GGUF",
    messages=[{"role": "user", "content": "Explain quantum entanglement in one sentence."}],
)
print(response.choices[0].message.content)
```

### Embeddings

```python
response = client.embeddings.create(
    model="nomic-embed-text-v1-GGUF",
    input="The quick brown fox",
)
vector = response.data[0].embedding
```

### Image generation

```python
response = client.images.generate(
    model="SD-Turbo",
    prompt="A sunset over the ocean, photorealistic",
    n=1,
    size="512x512",
)
image_b64 = response.data[0].b64_json
```

### Audio transcription

```python
with open("audio.wav", "rb") as f:
    response = client.audio.transcriptions.create(
        model="Whisper-Tiny",
        file=f,
    )
print(response.text)
```

### Suppress thinking on reasoning models

For models that default to extended chain-of-thought reasoning (e.g. Qwen3), you can disable thinking for faster, shorter responses:

```python
response = client.chat.completions.create(
    model="Qwen3-0.6B-GGUF",
    messages=[{"role": "user", "content": "What is 2 + 2?"}],
    extra_body={"enable_thinking": False},
)
```

---

## Handle streaming

Streaming returns tokens as they are generated rather than waiting for the full response.

```python
stream = client.chat.completions.create(
    model="Qwen3-0.6B-GGUF",
    messages=[{"role": "user", "content": "Write a haiku about autumn."}],
    stream=True,
)

for chunk in stream:
    delta = chunk.choices[0].delta
    if delta.content:
        print(delta.content, end="", flush=True)
```

**Connection drops:** If the connection drops mid-stream, restart the request from the beginning. Lemonade does not support resuming a partial stream. Use a retry loop with exponential backoff for resilience.

**First-token latency on cold start:** When a model is not yet loaded, the server loads it before emitting the first token. In a streaming context this means the connection stays open but silent for several seconds. Set a generous connect timeout (30 s or more) and a shorter read timeout on subsequent chunks. Pre-load the model explicitly with `POST /v1/load` if you need predictable latency.

---

## Manage models at runtime

### Pull (download) a model

```python
requests.post(f"{BASE_URL}/v1/pull", json={"model_name": "Qwen3-0.6B-GGUF"})
```

To track progress, use `stream=true` with `subscribe=false` for a server-owned background job:

```python
resp = requests.post(f"{BASE_URL}/v1/pull", json={
    "model_name": "Qwen3-0.6B-GGUF",
    "stream": True,
    "subscribe": False,
})
job_id = resp.json()["id"]

# Poll progress
import time
while True:
    jobs = requests.get(f"{BASE_URL}/v1/downloads").json()
    job = next((j for j in jobs if j["id"] == job_id), None)
    if job is None or job["complete"]:
        break
    print(f"{job['percent']}%")
    time.sleep(1)
```

See [`POST /v1/pull`](../api/lemonade.md#post-v1pull) and [`GET /v1/downloads`](../api/lemonade.md#get-v1downloads) for the full API.

### Load a model explicitly

Pre-loading eliminates cold-start latency on the first inference request:

```python
requests.post(f"{BASE_URL}/v1/load", json={"model_name": "Qwen3-0.6B-GGUF"})
```

Block until the model is ready by polling `/v1/health`:

```python
import time

requests.post(f"{BASE_URL}/v1/load", json={"model_name": "Qwen3-0.6B-GGUF"})
while True:
    health = requests.get(f"{BASE_URL}/v1/health").json()
    loaded = {m["model_name"] for m in health.get("all_models_loaded", [])}
    if "Qwen3-0.6B-GGUF" in loaded:
        break
    time.sleep(0.5)
```

### Unload a model

```python
requests.post(f"{BASE_URL}/v1/unload", json={"model_name": "Qwen3-0.6B-GGUF"})
```

Omit `model_name` to unload all loaded models.

---

## Error handling reference

All error responses are JSON with an `error` object:

```json
{"error": {"message": "Model not found: MyModel", "type": "not_found_error"}}
```

| HTTP status | Cause | Recovery |
|-------------|-------|----------|
| `400` | Malformed request (missing required fields, invalid JSON) | Fix the request payload |
| `401` | API key missing or wrong | Set the correct `Authorization: Bearer <key>` header |
| `404` | Model not found in registry | Pull the model first with `POST /v1/pull` |
| `422` | Model found but not downloaded | Pull the model first |
| `500` | Backend inference error or backend process crashed | Check `/v1/health` for loaded models; retry the request; check logs via `/logs/stream` |
| `503` | Server overloaded or model is still loading | Retry with exponential backoff |

**Context exceeded:** When the prompt exceeds the model's context window, llama.cpp returns a `400` or truncates silently depending on the `--ctx-shift` flag. Load the model with a larger `ctx_size` via `POST /v1/load` if you need longer contexts.

**Retry guidance:** Transient errors (network blips, cold-start timeout) are best retried with exponential backoff starting at 1 second, capped at 30 seconds, up to 5 attempts.

---

## Authentication

By default, Lemonade runs with no authentication. Set `LEMONADE_API_KEY` on the server to require a bearer token:

```bash
# Server side
export LEMONADE_API_KEY=my-secret-key
lemond

# Client side
curl http://localhost:13305/v1/models \
  -H "Authorization: Bearer my-secret-key"
```

With the OpenAI SDK:

```python
client = OpenAI(base_url="http://localhost:13305/v1", api_key="my-secret-key")
```

`LEMONADE_ADMIN_API_KEY` provides elevated access including management endpoints (`/v1/params`, `/v1/log-level`, `/internal/*`). Keep admin keys out of client-side code.

See [API Key and Security](../guide/configuration/README.md#api-key-and-security) for the full authentication hierarchy.

---

## Performance tips

**Pre-load models at app startup.** Use `POST /v1/load` during your app's initialization phase so the first user request gets a warm model. Poll `/v1/health` to confirm the model is ready before showing the UI.

**Use `max_loaded_models` to hold multiple models.** By default only one model per type can be loaded at a time. If your app uses an LLM and an embedding model together, set `max_loaded_models` to 2 or higher so switching between them doesn't evict and reload:

```bash
lemonade config set max_loaded_models=2
```

Or at runtime via the API:

```python
requests.post(f"{BASE_URL}/v1/params", json={"max_loaded_models": 2})
```

**Get performance stats after inference.** `GET /v1/stats` returns TTFT and tokens-per-second from the last request — useful for latency monitoring in your app.

**Stream for interactive UIs.** Streaming reduces perceived latency dramatically. Even a 5-second full-response generation feels fast when tokens appear immediately.

**Check system resources before loading large models.** `GET /v1/system-stats` returns current CPU, RAM, and VRAM usage. `GET /v1/system-info` returns device capabilities and which backends are installed.
