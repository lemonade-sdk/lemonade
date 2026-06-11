# Realtime WebSocket API

OpenAI Realtime-compatible WebSocket API for streaming audio transcription.

## Connecting

```
ws://HOST:13305/v1/realtime?model=MODEL_NAME
```

WebSocket upgrades are accepted **directly on the main HTTP port** (default 13305) — the same port as all REST endpoints. Accepted paths: `/realtime` and `/logs/stream`, bare or under any of the standard prefixes (`/v1`, `/v0`, `/api/v1`, `/api/v0`).

A dedicated WebSocket port also remains available for backward compatibility; it is OS-assigned and reported by `GET /v1/health` as `websocket_port`. New clients should prefer the main port.

**Auth** (only when `LEMONADE_API_KEY` is set): pass `?api_key=KEY` as a query parameter.

**Model**: pass `?model=NAME` where the model carries the `realtime-transcription` label (e.g. `Whisper-Tiny`, `Moonshine-Medium-Streaming`). Load it first via `POST /v1/load`.

## Audio format

Base64-encoded **PCM16, mono, 16 kHz**, little-endian, sent in `input_audio_buffer.append` events. Chunks of ~100 ms work well for live capture.

## Client → server events

| Event | Purpose |
|-------|---------|
| `{"type": "session.update", "session": {"model": "..."}}` | Configure the session after connecting. |
| `{"type": "input_audio_buffer.append", "audio": "<base64 pcm16>"}` | Stream an audio chunk. |
| `{"type": "input_audio_buffer.commit"}` | Finalize: flushes the in-flight utterance so a final transcript always follows. Send on manual stop. |
| `{"type": "input_audio_buffer.clear"}` | Discard buffered audio. |

## Server → client events

In typical order per utterance:

| Event | Payload | Meaning |
|-------|---------|---------|
| `session.created` | `session.id` | Sent immediately on connect. |
| `session.updated` | `session` | Ack for `session.update`. |
| `input_audio_buffer.speech_started` | — | Speech detected. |
| `conversation.item.input_audio_transcription.delta` | `delta` (string) | Interim text; **replaces** the previous interim for this utterance. |
| `input_audio_buffer.speech_stopped` | — | Speech ended. |
| `conversation.item.input_audio_transcription.completed` | `transcript` (string) | Final text for the utterance. |
| `input_audio_buffer.committed` | — | Ack for `commit`. |
| `input_audio_buffer.cleared` | — | Ack for `clear`. |
| `error` | `error.message` | Something went wrong. |

Streaming backends (Moonshine) emit deltas continuously while you speak; buffered backends (Whisper) emit interim/final results per VAD segment. The event contract is identical either way.

## Minimal client example

```python
import asyncio, base64, json, websockets

async def transcribe(pcm16_chunks):
    url = "ws://localhost:13305/v1/realtime?model=Moonshine-Medium-Streaming"
    async with websockets.connect(url) as ws:
        await ws.send(json.dumps({"type": "session.update", "session": {}}))
        async def reader():
            async for raw in ws:
                msg = json.loads(raw)
                if msg["type"].endswith("transcription.delta"):
                    print("interim:", msg["delta"])
                elif msg["type"].endswith("transcription.completed"):
                    print("final:", msg["transcript"])
        asyncio.ensure_future(reader())
        for chunk in pcm16_chunks:  # raw PCM16 mono 16 kHz bytes
            await ws.send(json.dumps({"type": "input_audio_buffer.append",
                                      "audio": base64.b64encode(chunk).decode()}))
            await asyncio.sleep(0.1)
        await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
        await asyncio.sleep(2)
```

## Upgrade path for existing clients

Clients written against the dedicated WebSocket port migrate in one step:

**Before** — discover the port, then connect to it:
```
GET /v1/health            → {"websocket_port": 9001, ...}
ws://HOST:9001/realtime?model=...
```

**After** — connect straight to the port you already use for REST:
```
ws://HOST:13305/v1/realtime?model=...
```

Notes:

1. No protocol changes — events, audio format, and auth are identical on both ports.
2. The `websocket_port` discovery step can be deleted, but keep it as a fallback if you must support Lemonade servers older than this release.
3. Remote setups get simpler: only one port to expose/forward/proxy. The upgrade also works through standard reverse proxies that pass `Upgrade: websocket` (nginx: `proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade";`).
4. Deprecation: the dedicated port stays for now and will be revisited after this has shipped in a few releases.

## Differences from OpenAI Realtime

1. Audio is **16 kHz** PCM16 (OpenAI defaults to 24 kHz); `session.input_audio_format` is currently ignored.
2. Events omit `event_id` / `item_id` / `audio_start_ms` envelope fields — correlate utterances by event order.
3. Transcription only: `response.create` and conversation/voice-out events are not supported.
