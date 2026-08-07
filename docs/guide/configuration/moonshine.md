# Moonshine Backend Options

Lemonade integrates [Moonshine](https://github.com/usefulsensors/moonshine) as a CPU-only **streaming speech-to-text** backend, using the [`moonshine-voice`](https://pypi.org/project/moonshine-voice/) streaming API. It complements the existing Whisper backend:

1. **True streaming.** Moonshine transcribes audio incrementally while you speak — interim results stream over the WebSocket Realtime API (`conversation.item.input_audio_transcription.delta`), with finals on segment completion. Whisper, by contrast, transcribes buffered VAD segments.
2. **Small and fast on CPU.** The streaming models range from ~30 MB (tiny) to ~250 MB (medium) and run in real-time on a laptop CPU — no GPU or NPU required.

## Available Backend

### CPU
- **Platform**: Windows x64, Linux x64/arm64, macOS arm64 (no Intel macOS or Windows-arm64 — `moonshine-voice` publishes no wheel for those)
- **Bundle**: a self-contained PyInstaller bundle from [lemonade-sdk/moonshine-server-rocm](https://github.com/lemonade-sdk/moonshine-server-rocm) with an embedded Python runtime and the `moonshine-voice` native libraries. No system Python install is required (or touched) on the host; Lemonade additionally sets `PYTHONNOUSERSITE=1` at launch.

## Install

```bash
lemonade backends install moonshine:cpu
```

Or via HTTP:
```bash
curl -X POST http://localhost:13305/api/v1/install \
  -H 'Content-Type: application/json' \
  -d '{"recipe": "moonshine", "backend": "cpu"}'
```

The bundle version is pinned in [`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json) (`moonshine.cpu`), with tags following the upstream library version (`moonshine0.0.62` = `moonshine-voice` 0.0.62). Bundles are built automatically by [lemonade-sdk/moonshine-server-rocm](https://github.com/lemonade-sdk/moonshine-server-rocm), a distribution-only repo that tracks `moonshine-voice` PyPI releases — no moonshine code is forked; the `main.py` wrapper in `tools/moonshine-server/` here is frozen together with the PyPI wheel into a self-contained bundle.

## Models

Three streaming models are registered in [`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json), downloading from [UsefulSensors/moonshine-streaming](https://huggingface.co/UsefulSensors/moonshine-streaming) on Hugging Face into the standard HF cache:

| Model | Checkpoint | Size |
|-------|-----------|------|
| `Moonshine-Tiny-Streaming` | `UsefulSensors/moonshine-streaming:onnx/tiny` | ~34 MB |
| `Moonshine-Small-Streaming` | `UsefulSensors/moonshine-streaming:onnx/small` | ~123 MB |
| `Moonshine-Medium-Streaming` | `UsefulSensors/moonshine-streaming:onnx/medium` | ~245 MB |

```bash
lemonade pull Moonshine-Medium-Streaming
```

To register your own:

```bash
lemonade pull user.MyMoonshine \
  --checkpoint main UsefulSensors/moonshine-streaming:onnx/medium \
  --recipe moonshine
```

## Use

### File transcription (OpenAI-compatible)

```bash
curl http://localhost:13305/v1/audio/transcriptions \
  -F model=Moonshine-Medium-Streaming \
  -F file=@speech.wav
```

### Realtime streaming

The WebSocket Realtime API streams interim and final transcripts while audio is being captured. Connect to `ws://HOST:PORT/realtime?model=...` directly on the main HTTP port (e.g. 13305); a dedicated WebSocket port (OS-assigned, surfaced via `GET /v1/health` `websocket_port`) also remains for backward compatibility. Send `input_audio_buffer.append` events with base64 PCM16 mono 16 kHz audio; Lemonade forwards them to the Moonshine subprocess over an internal line-delimited-JSON TCP bridge and relays the OpenAI Realtime events back:

| Event | When |
|-------|------|
| `input_audio_buffer.speech_started` | Moonshine opens a new speech line |
| `conversation.item.input_audio_transcription.delta` | Interim text for the current line (replaces previous interim) |
| `input_audio_buffer.speech_stopped` | The speech line ended |
| `conversation.item.input_audio_transcription.completed` | Final transcript for the line |
| `input_audio_buffer.committed` | Acknowledges `input_audio_buffer.commit`; the in-flight line is flushed so a final transcript always follows |

The desktop/web app's Transcription panel and the chat microphone button use this path automatically whenever the loaded transcription model carries the `realtime-transcription` label (all Moonshine models do).

## Tuning

Free-form CLI args can be appended to `moonshine-server` via `moonshine_args`:

```bash
lemonade config set moonshine_args="..."
```

(`--model-path`, `--model-arch`, `--port`, and `--tcp-port` are managed by Lemonade and rejected as custom args.)

## Known gotchas

- **English only.** The current `moonshine-streaming` checkpoints are English-only; the `language` request parameter is accepted but ignored.
- **Tokenizer conversion on first load.** The HF repo ships `tokenizer.json`; `moonshine-server` converts it to the `tokenizer.bin` format `moonshine-voice` expects on first load. The converted file is cached next to the model.
- **NPU coexistence.** Moonshine runs on CPU and does not participate in NPU exclusivity; it can stay loaded alongside FLM or RyzenAI models.
