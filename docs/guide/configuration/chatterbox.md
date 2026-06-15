# Chatterbox Backend Options

Lemonade integrates [Chatterbox](https://github.com/resemble-ai/chatterbox) (Resemble AI) as a **text-to-speech** backend, exposed through the OpenAI-compatible `/v1/audio/speech` endpoint (the same endpoint the Kokoro backend uses). Unlike Kokoro (CPU/Metal only), Chatterbox is a PyTorch model and supports **GPU acceleration** across vendors:

1. **GPU by default, CPU fallback.** Chatterbox auto-selects the best available device — CUDA (NVIDIA), ROCm (AMD, Linux), or Metal/MPS (Apple Silicon) — and falls back to CPU when no GPU is present. PyTorch's ROCm build drives AMD GPUs through the CUDA API, so a single bundle per device covers all GPU architectures.
2. **Expressive, multilingual, voice cloning.** Chatterbox supports emotion/exaggeration control, 23+ languages (multilingual variant), and zero-shot voice cloning from a reference clip.
3. **Byte-level streaming.** When the loaded Chatterbox build provides `generate_stream`, audio is emitted incrementally as raw PCM16 @ 24 kHz (`stream_format: "audio"`) for low time-to-first-audio; otherwise it falls back transparently to a single full-utterance response.

## Available Backends

Chatterbox auto-selects in this preference order (first available wins):

| Backend | Device | Platforms |
|---------|--------|-----------|
| `metal` | Apple Silicon GPU (MPS) | macOS arm64 |
| `cuda`  | NVIDIA GPU | Windows x64, Linux x64 |
| `rocm`  | AMD GPU (HIP via PyTorch ROCm) | Linux x64 |
| `cpu`   | CPU | Windows x64, Linux x64, macOS arm64 |

Each is a self-contained PyInstaller bundle from [lemonade-sdk/chatterbox-rocm](https://github.com/lemonade-sdk/chatterbox-rocm) with an embedded Python runtime, the device-appropriate PyTorch wheel, and the `chatterbox-tts` library. No system Python is required (or touched) on the host; Lemonade additionally sets `PYTHONNOUSERSITE=1` at launch.

## Install

The correct device bundle is installed automatically the first time a Chatterbox model is loaded. To install explicitly:

```bash
lemonade backends install chatterbox:cuda   # or rocm / metal / cpu
```

Or via HTTP:
```bash
curl -X POST http://localhost:13305/api/v1/install \
  -H 'Content-Type: application/json' \
  -d '{"recipe": "chatterbox", "backend": "cuda"}'
```

Bundle versions are pinned in [`backend_versions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/backend_versions.json) (`chatterbox.{cuda,rocm,metal,cpu}`), with tags following the upstream library version (`chatterbox0.1.7` = `chatterbox-tts` 0.1.7). Bundles are built automatically by [lemonade-sdk/chatterbox-rocm](https://github.com/lemonade-sdk/chatterbox-rocm), a distribution-only repo that tracks `chatterbox-tts` PyPI releases — no Chatterbox code is forked; the `main.py` wrapper in `tools/chatterbox-server/` here is frozen together with the PyPI wheel into a self-contained bundle.

## Models

Three variants are registered in [`server_models.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/resources/server_models.json), downloading from Hugging Face into the standard HF cache:

| Model | Variant | Checkpoint |
|-------|---------|-----------|
| `Chatterbox` | English (`ChatterboxTTS`) | `ResembleAI/chatterbox` |
| `Chatterbox-Multilingual` | 23+ languages (`ChatterboxMultilingualTTS`) | `ResembleAI/chatterbox` |
| `Chatterbox-Turbo` | Fast English w/ paralinguistic tags (`ChatterboxTurboTTS`) | `ResembleAI/chatterbox-turbo` |

```bash
lemonade pull Chatterbox
```

To register your own Chatterbox checkpoint (loaded via `from_local`):

```bash
lemonade pull user.MyChatterbox \
  --checkpoint main ResembleAI/chatterbox \
  --recipe chatterbox
```

## Use

### Speech synthesis (OpenAI-compatible)

```bash
curl http://localhost:13305/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"model": "Chatterbox", "input": "Hello from Lemonade.", "response_format": "mp3"}' \
  --output speech.mp3
```

`response_format` accepts `mp3` (default), `wav`, `pcm`, `flac`, and `opus`.

### Streaming

Request raw PCM streaming (24 kHz, signed 16-bit, little-endian):

```bash
curl http://localhost:13305/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"model": "Chatterbox", "input": "Streaming audio.", "stream_format": "audio"}' \
  --output speech.pcm
```

### Voice cloning and expressive controls

The OpenAI `voice` field is treated as a reference-audio path for zero-shot voice cloning when it points at an existing file (or pass `audio_prompt_path` explicitly). Chatterbox-specific controls `exaggeration`, `cfg_weight`, and `temperature` are passed through, and `language_id` selects the language for the multilingual variant:

```bash
curl http://localhost:13305/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"model": "Chatterbox-Multilingual", "input": "Bonjour.", "language_id": "fr", "exaggeration": 0.6}' \
  --output bonjour.mp3
```

## Tuning

Force a specific device (overriding auto-selection) via config or per-load:

```bash
lemonade config set chatterbox.backend=cpu
```

Free-form CLI args can be appended to `chatterbox-server` via `chatterbox_args`:

```bash
lemonade config set chatterbox_args="..."
```

(`--ckpt-dir`, `--variant`, `--device`, `--host`, and `--port` are managed by Lemonade and rejected as custom args.)

## Known gotchas

- **ROCm is Linux-only.** PyTorch publishes ROCm wheels for Linux only, so the `rocm` bundle is offered on Linux x64. On Windows, AMD GPUs fall back to the `cpu` bundle.
- **Large bundles.** Chatterbox ships a full PyTorch runtime; the GPU bundles are multi-gigabyte downloads. The first load also downloads the model weights (~2 GB) from Hugging Face.
- **GPU memory.** Chatterbox participates in the GPU LRU like other GPU models; on tight-VRAM systems it may evict (or be evicted by) an LLM. Use `--max-loaded-models` and per-model eviction settings to tune coexistence.
- **Streaming support is version-dependent.** Byte-level streaming uses Chatterbox's `generate_stream` when present in the installed build; otherwise the wrapper returns the full utterance as a single chunk over the same streaming contract.
