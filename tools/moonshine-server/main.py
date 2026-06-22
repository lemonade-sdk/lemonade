#!/usr/bin/env python3
"""moonshine-server — HTTP + WebSocket + TCP subprocess for Lemonade Moonshine backend.

Exposes:
  - whisper.cpp-compatible /inference endpoint for file-based transcription
  - /health endpoint for readiness checks
  - WebSocket /v1/audio/realtime for streaming transcription (OpenAI-compatible)
  - TCP line-delimited JSON for internal Lemonade streaming (backend-agnostic)

Usage:
    python main.py --model-path /path/to/model --model-arch 5 --port 8080 --ws-port 8081 --tcp-port 8082
"""

import argparse
import asyncio
import base64
import json
import os
import random
import string
import struct
import sys
import tempfile
import threading
import wave
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

# Force CPU-only execution
os.environ.setdefault("HIP_VISIBLE_DEVICES", "")
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")
os.environ.setdefault("ROCR_VISIBLE_DEVICES", "")

# moonshine_voice manages its own thread pool. If using a vendored/patched
# build with SetIntraOpNumThreads(1), thread count stays at ~2.
# The upstream pip package may use more threads; set ORT env vars if needed:
#   OMP_NUM_THREADS=1
#   ONNXRUNTIME_INTRA_OP_NUM_THREADS=1

import cgi
import io

try:
    import websockets
except ImportError:
    websockets = None

# Import moonshine_voice at module level.
# In the released lemonade-server package this module is bundled inside the
# moonshine-server tarball; during development it can be installed via
#   pip install moonshine_voice
try:
    from moonshine_voice import TranscriptEventListener
except ImportError as e:
    raise ImportError(
        "moonshine_voice is not available. "
        "If you are running from source, install it with: pip install moonshine_voice"
    ) from e


def convert_tokenizer_json_to_bin(
    tokenizer_json_path: str, tokenizer_bin_path: str
) -> None:
    """Convert a HuggingFace tokenizer.json to moonshine's tokenizer.bin format."""
    import json

    with open(tokenizer_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    model = data.get("model", {})
    vocab = model.get("vocab", {})
    if not vocab:
        raise ValueError(f"No vocabulary found in {tokenizer_json_path}")

    vocab_size = len(vocab)
    tokens = [b""] * vocab_size
    for token_str, token_id in vocab.items():
        if token_id < vocab_size:
            tokens[token_id] = token_str.encode("utf-8")

    added_tokens = data.get("added_tokens", [])
    for added in added_tokens:
        token_id = added.get("id")
        content = added.get("content", "")
        if token_id is not None and token_id < len(tokens):
            tokens[token_id] = content.encode("utf-8")
        elif token_id is not None and token_id >= len(tokens):
            tokens.extend([b""] * (token_id - len(tokens) + 1))
            tokens[token_id] = content.encode("utf-8")

    with open(tokenizer_bin_path, "wb") as f:
        for token_bytes in tokens:
            length = len(token_bytes)
            if length == 0:
                f.write(b"\x00")
            elif length < 128:
                f.write(bytes([length]))
                f.write(token_bytes)
            else:
                first_byte = (length % 128) + 128
                second_byte = length // 128
                f.write(bytes([first_byte, second_byte]))
                f.write(token_bytes)


def ensure_tokenizer_bin(model_path: str) -> None:
    """Ensure tokenizer.bin exists in model_path, converting from tokenizer.json if needed."""
    import os

    bin_path = os.path.join(model_path, "tokenizer.bin")
    if os.path.exists(bin_path):
        return
    json_path = os.path.join(model_path, "tokenizer.json")
    if not os.path.exists(json_path):
        raise FileNotFoundError(
            f"Neither tokenizer.bin nor tokenizer.json found in {model_path}"
        )
    print(
        f"[moonshine-server] Converting tokenizer.json -> tokenizer.bin ...",
        file=sys.stderr,
    )
    convert_tokenizer_json_to_bin(json_path, bin_path)
    print(f"[moonshine-server] Wrote {bin_path}", file=sys.stderr)


def load_model(model_path: str, model_arch: int):
    """Lazy import and load moonshine model."""
    from moonshine_voice import Transcriber, ModelArch

    ensure_tokenizer_bin(model_path)
    arch = ModelArch(model_arch)
    return Transcriber(model_path=model_path, model_arch=arch)


def pcm16_to_f32(pcm16_bytes: bytes) -> list:
    """Convert little-endian PCM16 bytes to float32 samples (range [-1, 1])."""
    count = len(pcm16_bytes) // 2
    if count == 0:
        return []
    ints = struct.unpack(f"<{count}h", pcm16_bytes)
    return [s / 32768.0 for s in ints]


def _generate_id(prefix: str = "sess_", length: int = 24) -> str:
    """Generate a random session ID."""
    chars = string.ascii_lowercase + string.digits
    return prefix + "".join(random.choice(chars) for _ in range(length))


class _EventSender:
    """Abstract base for sending events to a client."""

    def send(self, msg: dict):
        raise NotImplementedError

    def close(self):
        pass


class WebSocketEventSender(_EventSender):
    def __init__(self, loop: asyncio.AbstractEventLoop, websocket):
        self.loop = loop
        self.websocket = websocket
        self._closed = False

    def send(self, msg: dict):
        if self._closed:
            return
        try:
            asyncio.run_coroutine_threadsafe(
                self.websocket.send(json.dumps(msg)), self.loop
            )
        except Exception:
            self._closed = True

    def close(self):
        self._closed = True


class TcpEventSender(_EventSender):
    def __init__(self, writer: asyncio.StreamWriter):
        self.writer = writer
        self._closed = False
        self._loop = asyncio.get_running_loop()

    def send(self, msg: dict):
        if self._closed:
            return
        line = (json.dumps(msg) + "\n").encode("utf-8")

        def _write():
            if self._closed:
                return
            try:
                self.writer.write(line)
                asyncio.ensure_future(self._drain())
            except Exception:
                self._closed = True

        try:
            # Listener callbacks arrive on moonshine_voice's internal C++
            # thread; StreamWriter is not thread-safe, so schedule the whole
            # write+drain onto the event loop.
            self._loop.call_soon_threadsafe(_write)
        except Exception:
            self._closed = True

    async def _drain(self):
        try:
            await self.writer.drain()
        except Exception:
            self._closed = True

    def close(self):
        self._closed = True


class StreamingListener(TranscriptEventListener):
    """Bridge between moonshine_voice C++ callbacks and an event sender.

    The moonshine_voice library calls these methods from its internal C++ thread,
    so we use asyncio.run_coroutine_threadsafe() or asyncio.create_task() to
    safely schedule sends.
    """

    def __init__(self, sender: _EventSender):
        super().__init__()
        self.sender = sender
        self._current_text = ""
        self._suppress_line = False

    def discard_current_line(self):
        """input_audio_buffer.clear: drop the in-flight line silently while
        keeping the stream open. The next line emits events normally."""
        self._current_text = ""
        self._suppress_line = True

    def on_line_started(self, event):
        self._suppress_line = False
        self._current_text = ""
        # Moonshine opens a "line" when it starts hearing speech — the closest
        # analog to OpenAI's stream-level VAD event.
        self.sender.send({"type": "input_audio_buffer.speech_started"})

    def on_line_text_changed(self, event):
        if self._suppress_line:
            return
        text = event.line.text or ""
        self._current_text = text
        self.sender.send(
            {
                "type": "conversation.item.input_audio_transcription.delta",
                "delta": text,
            }
        )

    def on_line_completed(self, event):
        if self._suppress_line:
            self._suppress_line = False
            self._current_text = ""
            return
        text = event.line.text or ""
        # Line completion means the speech segment ended: emit the stream-level
        # stop event before the final transcript, mirroring the OpenAI ordering
        # (speech_stopped -> transcription.completed).
        self.sender.send({"type": "input_audio_buffer.speech_stopped"})
        self.sender.send(
            {
                "type": "conversation.item.input_audio_transcription.completed",
                "transcript": text,
            }
        )
        self._current_text = ""


async def _handle_streaming_session(reader, sender: _EventSender, transcriber_factory):
    """Core streaming session logic shared between WebSocket and TCP."""
    transcriber = transcriber_factory()
    stream = transcriber.create_stream(update_interval=0.5)
    listener = StreamingListener(sender)

    # Attach listener — moonshine_voice API supports add_listener on either
    # transcriber or stream depending on version. Try stream first, fall back.
    if hasattr(stream, "add_listener"):
        stream.add_listener(listener)
    elif hasattr(transcriber, "add_listener"):
        transcriber.add_listener(listener)

    stream.start()

    try:
        while True:
            if hasattr(reader, "recv"):
                # WebSocket path
                try:
                    message = await reader.recv()
                except websockets.exceptions.ConnectionClosed:
                    break
            else:
                # TCP path
                line = await reader.readline()
                if not line:
                    break
                message = line.decode("utf-8").strip()
                if not message:
                    continue

            try:
                msg = json.loads(message)
            except json.JSONDecodeError:
                continue

            msg_type = msg.get("type")

            if msg_type == "input_audio_buffer.append":
                audio_b64 = msg.get("audio", "")
                try:
                    pcm16_bytes = base64.b64decode(audio_b64)
                except Exception:
                    continue
                samples = pcm16_to_f32(pcm16_bytes)
                if samples:
                    stream.add_audio(samples, sample_rate=16000)

            elif msg_type == "input_audio_buffer.clear":
                # Discard the in-flight line but keep the stream open so
                # subsequent appends keep streaming
                listener.discard_current_line()
                sender.send({"type": "input_audio_buffer.cleared"})

            elif msg_type == "input_audio_buffer.commit":
                # Finalize the in-flight line so the client receives a final
                # transcript for audio preceding the commit (e.g. the user
                # stopped recording mid-sentence). moonshine_voice exposes the
                # finalizer under different names across versions.
                flushed = False
                for meth in ("flush", "finalize"):
                    if hasattr(stream, meth):
                        try:
                            getattr(stream, meth)()
                            flushed = True
                        except Exception:
                            pass
                        break
                sender.send({"type": "input_audio_buffer.committed"})
                if not flushed and listener._current_text:
                    # No finalizer available — synthesize the final transcript
                    # from the last interim so the client is never left waiting.
                    sender.send({"type": "input_audio_buffer.speech_stopped"})
                    sender.send(
                        {
                            "type": "conversation.item.input_audio_transcription.completed",
                            "transcript": listener._current_text,
                        }
                    )
                    listener._current_text = ""

            elif msg_type == "session.update":
                sender.send({"type": "session.updated", "session": {}})

    finally:
        sender.close()
        try:
            stream.stop()
        except Exception:
            pass
        try:
            stream.close()
        except Exception:
            pass
        # The transcriber is shared across sessions — do not close it here.


async def websocket_handler(websocket, transcriber_factory):
    """Handle a single WebSocket connection for realtime streaming."""
    session_id = _generate_id("sess_moonshine_")
    loop = asyncio.get_running_loop()

    sender = WebSocketEventSender(loop, websocket)
    sender.send({"type": "session.created", "session": {"id": session_id}})

    await _handle_streaming_session(websocket, sender, transcriber_factory)


async def tcp_handler(
    reader: asyncio.StreamReader, writer: asyncio.StreamWriter, transcriber_factory
):
    """Handle a single TCP connection for line-delimited JSON streaming."""
    sender = TcpEventSender(writer)
    await _handle_streaming_session(reader, sender, transcriber_factory)
    try:
        writer.close()
        await writer.wait_closed()
    except Exception:
        pass


def get_handler(transcriber, tcp_ready=None):
    class MoonshineHandler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            # Suppress default logging; Lemonade manages its own logs
            pass

        def do_GET(self):
            parsed = urlparse(self.path)
            if parsed.path == "/health":
                # Not ready until the TCP streaming listener is bound, so
                # Lemonade's readiness probe cannot race the daemon thread
                if tcp_ready is not None and not tcp_ready.is_set():
                    self.send_response(503)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    self.wfile.write(b'{"status":"starting"}')
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"status":"ok"}')
                return
            self.send_response(404)
            self.end_headers()

        def do_POST(self):
            parsed = urlparse(self.path)
            if parsed.path != "/inference":
                self.send_response(404)
                self.end_headers()
                return

            try:
                content_type = self.headers.get("Content-Type", "")
                if not content_type.startswith("multipart/form-data"):
                    self._error(400, "Expected multipart/form-data")
                    return

                # Parse multipart form data
                environ = {
                    "REQUEST_METHOD": "POST",
                    "CONTENT_TYPE": content_type,
                    "CONTENT_LENGTH": self.headers.get("Content-Length", "0"),
                }
                form = cgi.FieldStorage(
                    fp=self.rfile,
                    environ=environ,
                    keep_blank_values=True,
                )

                # Extract audio file
                if "file" not in form:
                    self._error(400, "Missing 'file' field")
                    return

                file_item = form["file"]
                audio_bytes = file_item.file.read()

                # Save to temp file (moonshine_voice.load_wav_file needs a path).
                # The suffix derives from the client-supplied filename: map it to
                # a constant so no client data ever reaches the path (CodeQL
                # py/path-injection).
                allowed_exts = {
                    ".wav": ".wav",
                    ".mp3": ".mp3",
                    ".m4a": ".m4a",
                    ".ogg": ".ogg",
                    ".flac": ".flac",
                    ".webm": ".webm",
                }
                client_ext = os.path.splitext(file_item.filename or "audio.wav")[
                    1
                ].lower()
                ext = allowed_exts.get(client_ext, ".wav")
                with tempfile.NamedTemporaryFile(suffix=ext, delete=False) as tmp:
                    tmp.write(audio_bytes)
                    tmp_path = tmp.name

                try:
                    from moonshine_voice import load_wav_file

                    try:
                        audio_data, sample_rate = load_wav_file(tmp_path)
                    except Exception as e:
                        self._error(400, f"Invalid audio file: {str(e)}")
                        return
                finally:
                    try:
                        os.unlink(tmp_path)
                    except Exception:
                        pass

                # Transcribe
                result = transcriber.transcribe_without_streaming(
                    audio_data, sample_rate
                )

                # Build OpenAI-compatible response
                text = " ".join(line.text for line in result.lines)

                response_format = form.getvalue("response_format", "json")
                if response_format == "text":
                    body = text.encode("utf-8")
                    content_type_out = "text/plain; charset=utf-8"
                elif response_format == "srt":
                    body = _to_srt(result).encode("utf-8")
                    content_type_out = "text/plain; charset=utf-8"
                elif response_format == "vtt":
                    body = _to_vtt(result).encode("utf-8")
                    content_type_out = "text/plain; charset=utf-8"
                else:
                    body = json.dumps({"text": text}).encode("utf-8")
                    content_type_out = "application/json"

                self.send_response(200)
                self.send_header("Content-Type", content_type_out)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            except Exception as e:
                self._error(500, str(e))

        def _error(self, code: int, message: str):
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            body = json.dumps({"error": message}).encode("utf-8")
            self.wfile.write(body)

    return MoonshineHandler


def _to_srt(result) -> str:
    lines = []
    for i, line in enumerate(result.lines, 1):
        start = _fmt_timestamp(line.start_time)
        end = _fmt_timestamp(line.start_time + line.duration)
        lines.append(f"{i}\n{start} --> {end}\n{line.text}\n")
    return "\n".join(lines)


def _to_vtt(result) -> str:
    lines = ["WEBVTT\n"]
    for line in result.lines:
        start = _fmt_timestamp(line.start_time, vtt=True)
        end = _fmt_timestamp(line.start_time + line.duration, vtt=True)
        lines.append(f"{start} --> {end}\n{line.text}\n")
    return "\n".join(lines)


def _fmt_timestamp(seconds: float, vtt: bool = False) -> str:
    """Format seconds as SRT/VTT timestamp."""
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    secs = int(seconds % 60)
    millis = int((seconds % 1) * 1000)
    if vtt:
        return f"{hours:02d}:{minutes:02d}:{secs:02d}.{millis:03d}"
    return f"{hours:02d}:{minutes:02d}:{secs:02d},{millis:03d}"


def run_websocket_server(host: str, port: int, transcriber_factory):
    """Run the WebSocket server in a dedicated asyncio event loop (thread target)."""

    async def _serve():
        async def _handler(websocket):
            await websocket_handler(websocket, transcriber_factory)

        # Suppress websockets library logging noise
        import logging

        logging.getLogger("websockets").setLevel(logging.WARNING)

        async with websockets.serve(_handler, host, port):
            print(
                f"[moonshine-server] WebSocket listening on ws://{host}:{port}",
                file=sys.stderr,
                flush=True,
            )
            await asyncio.Future()  # Run forever

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(_serve())


def run_tcp_server(host: str, port: int, transcriber_factory, ready_event=None):
    """Run the TCP line-delimited JSON server in a dedicated asyncio event loop (thread target)."""

    async def _serve():
        server = await asyncio.start_server(
            lambda r, w: tcp_handler(r, w, transcriber_factory), host, port
        )
        print(
            f"[moonshine-server] TCP listening on {host}:{port}",
            file=sys.stderr,
            flush=True,
        )
        if ready_event is not None:
            ready_event.set()
        async with server:
            await server.serve_forever()

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(_serve())


def main():
    parser = argparse.ArgumentParser(
        description="Moonshine HTTP + WebSocket + TCP server for Lemonade"
    )
    parser.add_argument("--model-path", required=True, help="Path to model directory")
    parser.add_argument(
        "--model-arch",
        type=int,
        default=5,
        help="Model architecture enum value (default: 5)",
    )
    parser.add_argument("--port", type=int, default=8080, help="HTTP server port")
    parser.add_argument(
        "--ws-port",
        type=int,
        default=None,
        help="WebSocket server port (default: HTTP port + 1)",
    )
    parser.add_argument(
        "--tcp-port",
        type=int,
        default=None,
        help="TCP line-delimited JSON port (default: HTTP port + 2)",
    )
    args = parser.parse_args()

    ws_port = args.ws_port if args.ws_port is not None else args.port + 1
    tcp_port = args.tcp_port if args.tcp_port is not None else args.port + 2

    model_path = args.model_path

    print(
        f"[moonshine-server] Loading model from {model_path} (arch={args.model_arch})...",
        file=sys.stderr,
    )
    transcriber = load_model(model_path, args.model_arch)
    print(
        f"[moonshine-server] Model loaded. HTTP={args.port} WS={ws_port} TCP={tcp_port}",
        file=sys.stderr,
    )

    # Share the loaded transcriber across connections; each streaming session
    # gets its own stream via create_stream(). Loading a fresh model per
    # connection would multiply load time and memory per realtime client.
    def transcriber_factory():
        return transcriber

    tcp_ready = threading.Event()
    handler = get_handler(transcriber, tcp_ready)
    http_server = HTTPServer(("127.0.0.1", args.port), handler)

    # Start WebSocket server in a daemon thread
    ws_thread = threading.Thread(
        target=run_websocket_server,
        args=("127.0.0.1", ws_port, transcriber_factory),
        daemon=True,
    )
    ws_thread.start()

    # Start TCP server in a daemon thread
    tcp_thread = threading.Thread(
        target=run_tcp_server,
        args=("127.0.0.1", tcp_port, transcriber_factory, tcp_ready),
        daemon=True,
    )
    tcp_thread.start()

    try:
        http_server.serve_forever()
    except KeyboardInterrupt:
        print("[moonshine-server] Shutting down...", file=sys.stderr)
    finally:
        transcriber.close()
        http_server.server_close()


if __name__ == "__main__":
    main()
