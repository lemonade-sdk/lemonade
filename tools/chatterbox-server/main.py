#!/usr/bin/env python3
"""chatterbox-server — thin OpenAI-compatible TTS HTTP wrapper around Resemble
AI's Chatterbox models, consumed by Lemonade's ``chatterbox`` backend.

No Chatterbox code is vendored here. ``chatterbox-tts`` is installed from PyPI
at build time and frozen, together with this wrapper, into a self-contained
PyInstaller bundle by the ``lemonade-sdk/chatterbox-rocm`` distribution repo —
no system Python is required (or touched) on user machines.

The wrapper exposes a single inference endpoint, ``POST /v1/audio/speech``,
matching the OpenAI text-to-speech contract that Lemonade's Router forwards to
(identical to the Kokoro backend's contract), plus ``GET /`` and
``GET /health`` readiness probes.

Device selection: ``cuda`` (also covers AMD ROCm — PyTorch's ROCm build reports
``torch.cuda.is_available()``), then Apple ``mps`` (Metal), else ``cpu``. A
``--device`` override is honored; ``auto`` (the default) picks the best
available, giving "GPU by default, CPU fallback".

Streaming: when the client requests ``stream_format: "audio"`` (or
``stream: true``), audio is emitted as raw little-endian PCM16 at 24 kHz —
exactly the format Lemonade advertises (``audio/l16;rate=24000``) and the
native sample rate of Chatterbox (S3GEN_SR = 24000), so no resampling is
needed. Byte-level streaming uses Chatterbox's ``generate_stream`` when the
installed version provides it (detected at runtime); otherwise it falls back to
a single full-utterance chunk, keeping the HTTP contract identical.
"""

import argparse
import io
import json
import os
import struct
import sys
import threading
import wave

import numpy as np

# Chatterbox is built on PyTorch; importing torch up front lets us probe the
# available accelerators before the (slower) model import.
import torch


# Chatterbox's S3Gen vocoder samples at 24 kHz. This matches OpenAI's "pcm"
# response format (audio/l16;rate=24000;little-endian), so PCM streaming is a
# zero-resample passthrough.
SAMPLE_RATE = 24000

# Set once the model finishes loading; gates the /health readiness probe.
_READY = threading.Event()
_MODEL = None
_VARIANT = "english"
# Serializes generation: a single Chatterbox model instance is not safe to call
# concurrently from multiple request threads.
_GEN_LOCK = threading.Lock()


# --------------------------------------------------------------------------- #
# Device selection
# --------------------------------------------------------------------------- #
def pick_device(requested):
    """Resolve the torch device string.

    ``auto`` prefers CUDA (NVIDIA, and AMD via PyTorch's ROCm build, which
    masquerades as CUDA), then Apple MPS (Metal), then CPU.
    """
    if requested and requested != "auto":
        return requested
    if torch.cuda.is_available():
        return "cuda"
    mps = getattr(torch.backends, "mps", None)
    if mps is not None and mps.is_available():
        return "mps"
    return "cpu"


# --------------------------------------------------------------------------- #
# Model loading
# --------------------------------------------------------------------------- #
def load_model(variant, ckpt_dir, device):
    """Instantiate the requested Chatterbox variant on ``device``.

    Prefers ``from_local(ckpt_dir, device)`` so the model is read from the
    checkpoint directory Lemonade already downloaded into the Hugging Face
    cache; falls back to ``from_pretrained(device)`` (which downloads on
    demand) if a local load is not possible.
    """
    if variant == "multilingual":
        from chatterbox.mtl_tts import ChatterboxMultilingualTTS as Model
    elif variant == "turbo":
        from chatterbox.tts_turbo import ChatterboxTurboTTS as Model
    else:
        from chatterbox.tts import ChatterboxTTS as Model

    if ckpt_dir and os.path.isdir(ckpt_dir):
        try:
            return Model.from_local(ckpt_dir, device)
        except Exception as exc:  # noqa: BLE001 - fall back to hub download
            print(
                f"[chatterbox-server] from_local({ckpt_dir!r}) failed ({exc}); "
                "falling back to from_pretrained",
                file=sys.stderr,
                flush=True,
            )
    return Model.from_pretrained(device)


# --------------------------------------------------------------------------- #
# Audio conversion / encoding
# --------------------------------------------------------------------------- #
def to_pcm16(wav):
    """Convert a Chatterbox waveform (torch tensor or ndarray, float32 in
    [-1, 1], shape (1, N) or (N,)) to little-endian signed-16-bit PCM bytes."""
    if hasattr(wav, "detach"):
        wav = wav.detach().to("cpu").float().numpy()
    wav = np.asarray(wav, dtype=np.float32).reshape(-1)
    wav = np.clip(wav, -1.0, 1.0)
    return (wav * 32767.0).astype("<i2").tobytes()


def pcm_to_wav(pcm_bytes):
    """Wrap raw PCM16 mono @ SAMPLE_RATE in a RIFF/WAVE container."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def encode_full(pcm_bytes, fmt):
    """Encode complete PCM16 audio into the requested OpenAI response_format.

    Returns ``(encoded_bytes, ok)``. ``ok`` is False when no encoder for the
    requested format is available, so the caller can surface a clean error.
    """
    fmt = (fmt or "mp3").lower()

    if fmt == "pcm":
        return pcm_bytes, True
    if fmt == "wav":
        return pcm_to_wav(pcm_bytes), True

    if fmt == "mp3":
        try:
            import lameenc

            enc = lameenc.Encoder()
            enc.set_bit_rate(128)
            enc.set_in_sample_rate(SAMPLE_RATE)
            enc.set_channels(1)
            enc.set_quality(2)
            return enc.encode(pcm_bytes) + enc.flush(), True
        except Exception as exc:  # noqa: BLE001
            print(f"[chatterbox-server] mp3 encode failed: {exc}", file=sys.stderr, flush=True)
            return b"", False

    # flac / ogg-opus via libsndfile (soundfile).
    if fmt in ("flac", "opus"):
        try:
            import soundfile as sf

            samples = np.frombuffer(pcm_bytes, dtype="<i2")
            buf = io.BytesIO()
            if fmt == "flac":
                sf.write(buf, samples, SAMPLE_RATE, format="FLAC")
            else:
                sf.write(buf, samples, SAMPLE_RATE, format="OGG", subtype="OPUS")
            return buf.getvalue(), True
        except Exception as exc:  # noqa: BLE001
            print(f"[chatterbox-server] {fmt} encode failed: {exc}", file=sys.stderr, flush=True)
            return b"", False

    # aac and anything else: best effort via PyAV if present.
    try:
        import av  # noqa: F401

        return _encode_via_av(pcm_bytes, fmt), True
    except Exception as exc:  # noqa: BLE001
        print(f"[chatterbox-server] no encoder for format '{fmt}': {exc}", file=sys.stderr, flush=True)
        return b"", False


def _encode_via_av(pcm_bytes, fmt):
    """Encode PCM16 into ``fmt`` using PyAV (ffmpeg). Used for aac/opus/etc."""
    import av

    container_fmt = {"aac": "adts", "opus": "ogg", "mp3": "mp3", "flac": "flac"}.get(fmt, fmt)
    out = io.BytesIO()
    container = av.open(out, mode="w", format=container_fmt)
    stream = container.add_stream(fmt, rate=SAMPLE_RATE)
    stream.layout = "mono"

    samples = np.frombuffer(pcm_bytes, dtype="<i2").reshape(1, -1)
    frame = av.AudioFrame.from_ndarray(samples, format="s16", layout="mono")
    frame.rate = SAMPLE_RATE
    for packet in stream.encode(frame):
        container.mux(packet)
    for packet in stream.encode(None):  # flush
        container.mux(packet)
    container.close()
    return out.getvalue()


# --------------------------------------------------------------------------- #
# Generation
# --------------------------------------------------------------------------- #
def build_gen_kwargs(body):
    """Map request fields to Chatterbox generate() kwargs.

    OpenAI's ``voice`` is treated as an optional reference-audio path for voice
    cloning when it points at an existing file (also accepts the explicit
    ``audio_prompt_path``). Chatterbox expressive controls and the multilingual
    ``language_id`` are passed through when provided.
    """
    kwargs = {}

    prompt = body.get("audio_prompt_path")
    voice = body.get("voice")
    if not prompt and isinstance(voice, str) and os.path.isfile(voice):
        prompt = voice
    if prompt:
        kwargs["audio_prompt_path"] = prompt

    for key in ("exaggeration", "cfg_weight", "temperature"):
        if key in body and body[key] is not None:
            kwargs[key] = body[key]

    if _VARIANT == "multilingual":
        lang = body.get("language_id") or body.get("language")
        if lang:
            kwargs["language_id"] = lang

    return kwargs


def generate_full(text, gen_kwargs):
    """Run a full (non-streaming) synthesis and return PCM16 bytes."""
    with _GEN_LOCK:
        wav = _MODEL.generate(text, **gen_kwargs)
    return to_pcm16(wav)


def iter_pcm_chunks(text, gen_kwargs, chunk_size):
    """Yield PCM16 byte chunks as Chatterbox produces them.

    Uses ``generate_stream`` when the installed Chatterbox version exposes it
    (true byte-level streaming); otherwise falls back to one full-utterance
    chunk so the streaming HTTP contract still holds.
    """
    with _GEN_LOCK:
        if hasattr(_MODEL, "generate_stream"):
            stream_kwargs = dict(gen_kwargs)
            if chunk_size:
                stream_kwargs.setdefault("chunk_size", chunk_size)
            try:
                for item in _MODEL.generate_stream(text, **stream_kwargs):
                    # Some versions yield (audio_chunk, metrics); others just audio.
                    chunk = item[0] if isinstance(item, (tuple, list)) else item
                    if chunk is None:
                        continue
                    pcm = to_pcm16(chunk)
                    if pcm:
                        yield pcm
                return
            except TypeError:
                # Signature mismatch (e.g. no chunk_size kwarg) — retry plainly.
                for item in _MODEL.generate_stream(text, **gen_kwargs):
                    chunk = item[0] if isinstance(item, (tuple, list)) else item
                    if chunk is None:
                        continue
                    pcm = to_pcm16(chunk)
                    if pcm:
                        yield pcm
                return

        # Fallback: no streaming support in this build.
        wav = _MODEL.generate(text, **gen_kwargs)
    yield to_pcm16(wav)


# --------------------------------------------------------------------------- #
# HTTP server
# --------------------------------------------------------------------------- #
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer  # noqa: E402


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quieter logs; Lemonade captures stdout
        if os.environ.get("CHATTERBOX_VERBOSE"):
            super().log_message(fmt, *args)

    # -- helpers ----------------------------------------------------------- #
    def _send_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, status, message, etype="invalid_request_error"):
        self._send_json(status, {"error": {"message": message, "type": etype}})

    def _write_chunk(self, data):
        """Write one HTTP/1.1 chunked-transfer chunk."""
        self.wfile.write(f"{len(data):X}\r\n".encode("ascii"))
        self.wfile.write(data)
        self.wfile.write(b"\r\n")

    # -- routing ----------------------------------------------------------- #
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/health"):
            if _READY.is_set():
                self._send_json(200, {"status": "ok"})
            else:
                self._send_json(503, {"status": "starting"})
        else:
            self._send_error(404, "Not found")

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path not in ("/v1/audio/speech", "/audio/speech", "/api/v1/audio/speech"):
            self._send_error(404, "Not found")
            return

        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length) or b"{}")
        except Exception as exc:  # noqa: BLE001
            self._send_error(400, f"Invalid JSON body: {exc}")
            return

        text = body.get("input")
        if not text:
            self._send_error(400, "Missing 'input' field")
            return

        if not _READY.is_set():
            self._send_error(503, "Model is still loading", etype="server_error")
            return

        streaming = bool(body.get("stream")) or "stream_format" in body
        stream_format = body.get("stream_format")  # Lemonade only sends "audio"
        response_format = (body.get("response_format") or "mp3").lower()

        try:
            gen_kwargs = build_gen_kwargs(body)
        except Exception as exc:  # noqa: BLE001
            self._send_error(400, f"Invalid generation parameters: {exc}")
            return

        try:
            if streaming and stream_format == "sse":
                self._stream_sse(text, gen_kwargs, body)
            elif streaming:
                self._stream_audio(text, gen_kwargs, body)
            else:
                self._send_full(text, gen_kwargs, response_format)
        except BrokenPipeError:
            pass  # client disconnected mid-stream
        except Exception as exc:  # noqa: BLE001
            print(f"[chatterbox-server] generation error: {exc}", file=sys.stderr, flush=True)
            # Headers may already be sent in streaming paths; best effort.
            try:
                self._send_error(500, str(exc), etype="server_error")
            except Exception:  # noqa: BLE001
                pass

    # -- response strategies ----------------------------------------------- #
    def _send_full(self, text, gen_kwargs, response_format):
        pcm = generate_full(text, gen_kwargs)
        data, ok = encode_full(pcm, response_format)
        if not ok:
            self._send_error(400, f"Unsupported audio format: {response_format}")
            return
        mime = {
            "mp3": "audio/mpeg",
            "opus": "audio/opus",
            "aac": "audio/aac",
            "flac": "audio/flac",
            "wav": "audio/wav",
            "pcm": "audio/l16;rate=24000;endianness=little-endian",
        }.get(response_format, "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _stream_audio(self, text, gen_kwargs, body):
        """Chunked raw PCM16 @ 24 kHz — Lemonade's stream_format: "audio"."""
        chunk_size = int(body.get("chunk_size", 0) or 0)
        self.send_response(200)
        self.send_header("Content-Type", "audio/l16;rate=24000;endianness=little-endian")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        for pcm in iter_pcm_chunks(text, gen_kwargs, chunk_size):
            self._write_chunk(pcm)
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _stream_sse(self, text, gen_kwargs, body):
        """OpenAI Server-Sent-Events streaming (speech.audio.delta/done)."""
        import base64

        chunk_size = int(body.get("chunk_size", 0) or 0)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        def sse(obj):
            self._write_chunk(("data: " + json.dumps(obj) + "\n\n").encode("utf-8"))
            self.wfile.flush()

        for pcm in iter_pcm_chunks(text, gen_kwargs, chunk_size):
            sse({"type": "speech.audio.delta", "audio": base64.b64encode(pcm).decode("ascii")})
        sse({"type": "speech.audio.done"})
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


def main():
    global _MODEL, _VARIANT

    parser = argparse.ArgumentParser(description="OpenAI-compatible Chatterbox TTS server")
    parser.add_argument("--ckpt-dir", default="", help="Local checkpoint directory (HF snapshot)")
    parser.add_argument(
        "--variant",
        default="english",
        choices=["english", "multilingual", "turbo"],
        help="Chatterbox model class to load",
    )
    parser.add_argument(
        "--device",
        default="auto",
        choices=["auto", "cuda", "mps", "cpu"],
        help="Inference device (auto = GPU if available, else CPU)",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    _VARIANT = args.variant
    device = pick_device(args.device)
    print(
        f"[chatterbox-server] variant={args.variant} device={device} "
        f"ckpt_dir={args.ckpt_dir or '<hub>'}",
        flush=True,
    )

    # Start the HTTP server first so /health answers "starting" during the
    # (potentially slow) model load, then load the model in this thread.
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"[chatterbox-server] listening on {args.host}:{args.port}", flush=True)

    _MODEL = load_model(args.variant, args.ckpt_dir, device)
    _READY.set()
    print("[chatterbox-server] model ready", flush=True)

    try:
        threading.Event().wait()  # block forever; serving happens in the thread
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    main()
