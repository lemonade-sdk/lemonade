# Request Cancellation

Lemonade Server exposes endpoints to cancel in-flight inference requests and to list requests that are currently active. Cancellation is cooperative: the server sets an atomic flag that aborts the libcurl transfer to the backend subprocess, which tears down the TCP connection and stops inference.

## Endpoints

All endpoints follow the quad-prefix convention — each works under `/api/v0/`, `/api/v1/`, `/v0/`, and `/v1/`.

### Cancel a request

```
POST /v1/requests/{request_id}/cancel
```

**Path parameters**

| Name | Type | Description |
|------|------|-------------|
| `request_id` | string | The ID returned in the `X-Request-Id` response header of the original request, or a client-supplied ID sent via the `X-Request-Id` request header. |

**Request body**: none.

**Response — 200 OK**

```json
{
  "status": "cancelled",
  "request_id": "a1b2c3d4e5f6..."
}
```

**Response — 404 Not Found**

```json
{
  "error": {
    "message": "Request not found or already completed",
    "type": "not_found",
    "request_id": "a1b2c3d4e5f6..."
  }
}
```

A 404 is returned when the request ID was never registered, has already finished, or was already cancelled and removed from the registry.

**Response — 400 Bad Request**

```json
{
  "error": {
    "message": "Missing request_id in path",
    "type": "invalid_request_error"
  }
}
```

**Response — 503 Service Unavailable**

```json
{
  "error": {
    "message": "Server not ready",
    "type": "server_error"
  }
}
```

### List active requests

```
GET /v1/requests
```

**Response — 200 OK**

Returns a JSON array. Each entry represents a request currently registered in the server:

```json
[
  {
    "request_id": "a1b2c3d4e5f6...",
    "model_name": "Qwen3-4B-Q8_0-GGUF",
    "endpoint": "streaming",
    "is_streaming": true,
    "elapsed_ms": 4521
  }
]
```

| Field | Type | Description |
|-------|------|-------------|
| `request_id` | string | Unique identifier for the request. |
| `model_name` | string | Model the request is running against. |
| `endpoint` | string | Internal routing category (currently always `"streaming"`). |
| `is_streaming` | bool | Whether the request is a streaming (SSE) response. |
| `elapsed_ms` | integer | Milliseconds since the request was registered. |

## Client examples

### JavaScript (AbortController + fetch)

```js
const BASE = "http://localhost:13305";

async function streamChat(prompt, signal) {
  const res = await fetch(`${BASE}/v1/chat/completions`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: "Qwen3-4B-Q8_0-GGUF",
      messages: [{ role: "user", content: prompt }],
      stream: true,
    }),
    signal,
  });

  const requestId = res.headers.get("X-Request-Id");
  const reader = res.body.getReader();
  const decoder = new TextDecoder();

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    process.stdout.write(decoder.decode(value));
  }

  return requestId;
}

// Cancel from another part of your app:
async function cancelRequest(requestId) {
  await fetch(`${BASE}/v1/requests/${requestId}/cancel`, { method: "POST" });
}

// Usage with AbortController for client-side abort:
const controller = new AbortController();
const requestIdPromise = streamChat("Tell me a long story.", controller.signal);

// Later, cancel by request ID (server-side cancellation):
const requestId = await requestIdPromise;
await cancelRequest(requestId);

// Or abort the fetch entirely (client-side disconnect):
controller.abort();
```

### Python (httpx async)

```python
import asyncio
import httpx

BASE = "http://localhost:13305"

async def stream_chat(prompt: str):
    async with httpx.AsyncClient(timeout=None) as client:
        async with client.stream(
            "POST",
            f"{BASE}/v1/chat/completions",
            headers={"Content-Type": "application/json"},
            json={
                "model": "Qwen3-4B-Q8_0-GGUF",
                "messages": [{"role": "user", "content": prompt}],
                "stream": True,
            },
        ) as res:
            request_id = res.headers.get("X-Request-Id")
            async for line in res.aiter_lines():
                print(line)
            return request_id

async def cancel_request(request_id: str):
    async with httpx.AsyncClient() as client:
        await client.post(f"{BASE}/v1/requests/{request_id}/cancel")

async def list_active():
    async with httpx.AsyncClient() as client:
        res = await client.get(f"{BASE}/v1/requests")
        return res.json()

async def main():
    task = asyncio.create_task(stream_chat("Tell me a long story."))
    # Give the request time to start and capture the request ID
    await asyncio.sleep(2)
    active = await list_active()
    print("Active requests:", active)

    # Cancel the first active request
    if active:
        await cancel_request(active[0]["request_id"])

asyncio.run(main())
```

### curl

```bash
# Start a streaming request, capture the X-Request-Id header
curl -N -D headers.txt \
  -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "Qwen3-4B-Q8_0-GGUF",
    "messages": [{"role": "user", "content": "Write a long story."}],
    "stream": true
  }' &

# Extract the request ID from response headers
sleep 2
REQUEST_ID=$(grep -i "X-Request-Id" headers.txt | tr -d '\r' | awk '{print $2}')

# List active requests
curl http://localhost:13305/v1/requests

# Cancel it
curl -X POST "http://localhost:13305/v1/requests/${REQUEST_ID}/cancel"
```

## Architecture

Cancellation flows through four layers:

```
Client ──POST /v1/requests/{id}/cancel──▶ Server (handle_request_cancel)
                                               │
                                               ▼
                                        RequestRegistry
                                        cancel_request(id)
                                               │
                                    sets atomic<bool> = true
                                               │
                                               ▼
                                        libcurl XFERINFOFUNCTION
                                        cancel_progress_callback
                                        returns 1 → CURLE_ABORTED_BY_CALLBACK
                                               │
                                               ▼
                                        TCP connection to backend
                                        subprocess is torn down
                                               │
                                               ▼
                                        Backend subprocess (llama-server,
                                        flm, etc.) observes connection
                                        drop and stops inference
```

1. **`RequestRegistry`** holds a `std::map<std::string, ActiveRequest>` protected by a mutex. Each `ActiveRequest` owns a `std::shared_ptr<std::atomic<bool>>` cancel flag, created at registration time.
2. **`ActiveRequestGuard`** (RAII) is held on the stack during the streaming call. When the guard is destroyed (request completes or throws), the entry is automatically unregistered.
3. **libcurl progress callback** (`cancel_progress_callback` in `http_client.cpp`) receives the cancel flag as `CURLOPT_XFERINFODATA`. When the flag is set, it returns `1`, which causes libcurl to abort with `CURLE_ABORTED_BY_CALLBACK`.
4. **`StreamingProxy::forward_sse_stream`** detects cancellation and writes a well-formed SSE error event followed by `[DONE]` so the client's SSE parser terminates cleanly:

```
data: {"error":{"message":"Request cancelled by user","type":"request_cancelled","code":"cancelled"}}

data: [DONE]
```

## Request ID

### Server-generated IDs

When no `X-Request-Id` request header is present, the server generates a 32-character lowercase hex string using a thread-local `std::mt19937`. The generated ID is:

- Returned in the `X-Request-Id` **response header** for streaming responses.
- Used as the registry key for the duration of the request.

### Client-provided IDs

Clients may supply their own ID via the `X-Request-Id` request header. Accepted values must be non-empty and at most 128 characters. The server uses the client-provided value as-is unless a collision occurs, in which case a numeric suffix is appended (`<id>-2`, `<id>-3`, ...) to guarantee uniqueness in the registry.

Client-provided IDs are useful when:

- The client already tracks requests with its own correlation IDs.
- Multiple clients need to agree on a request ID out-of-band.
- Idempotent retry logic needs a stable identifier.

### Which endpoints set X-Request-Id

The `X-Request-Id` response header is set on streaming responses for:

- `POST /v1/chat/completions` (when `stream: true`)
- `POST /v1/completions` (when `stream: true`)
- `POST /v1/responses` (when `stream: true`)

Non-streaming (synchronous) requests do not currently register in the request registry and cannot be cancelled via the API. They still complete normally when the client disconnects — see [Client disconnect](#client-disconnect).

## Behavior notes

### Streaming vs non-streaming

Cancellation is supported for **streaming (SSE) requests** only. Non-streaming requests block synchronously in `HttpClient::post` without registering in the `RequestRegistry`, so `POST /v1/requests/{id}/cancel` returns 404 for them. Non-streaming requests terminate naturally when the client closes the TCP connection.

### Already-completed requests

A request that finished normally is unregistered by the `ActiveRequestGuard` destructor before the response is fully flushed. A cancel request arriving after that gets 404 — `"Request not found or already completed"`.

### Non-existent IDs

Same 404 response. There is no way to distinguish "never existed" from "already completed" — both are intentional.

### Client disconnect

When the client closes its TCP connection mid-stream, `sink.write()` in the streaming proxy returns `false`, which causes the stream callback to return `false`. libcurl then aborts with `CURLE_WRITE_ERROR`. The server logs a client-disconnect warning and tears down the backend connection. No explicit cancel call is needed — disconnecting is itself a cancellation.

### ID collisions

If a client re-sends the same `X-Request-Id` while the previous request with that ID is still active, the registry appends a numeric suffix (`-2`, `-3`, ...) to the new request's registry key. The `X-Request-Id` response header reflects the suffixed key, so the client can target the correct request when cancelling.

### Shutdown

`POST /internal/shutdown` calls `router_->cancel_all_requests()` before unloading models. Every in-flight streaming request receives a cancel signal.

## Limitations

- **Streaming only.** Non-streaming (synchronous) inference requests are not registered and cannot be cancelled through the API.
- **Best-effort.** The cancel flag is checked at libcurl progress-callback granularity and at each SSE chunk boundary. A request that is blocked inside a backend subprocess waiting for GPU compute will not stop until the backend writes to the connection or the connection is torn down at the transport level.
- **No partial output rollback.** Tokens already streamed to the client before cancellation cannot be recalled. The client receives the cancel SSE event and `[DONE]`, and is responsible for discarding partial output.
- **No cancel-all via public API.** `cancel_all` is internal only (`POST /internal/shutdown`). To cancel multiple requests, iterate over `GET /v1/requests` and cancel each individually.
- **No cancellation of model loads, downloads, or backend installs.** The registry tracks inference requests only. Model pull/install/delete operations have their own cancellation mechanisms (e.g., `cancel_download_jobs()` on shutdown).
- **No retry on cancel.** A cancelled request is terminal. The client must issue a new request from scratch.
- **No cross-server cancellation.** The registry is in-process. If multiple `lemond` instances are running (e.g., on different ports), a cancel call targets only the instance that received the POST.
