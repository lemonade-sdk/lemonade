#!/usr/bin/env python3
"""Quick smoke test for moonshine-server WebSocket + TCP streaming."""

import asyncio
import base64
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

SAMPLE_RATE = 16000


def read_wav_pcm16(path: str) -> bytes:
    with wave.open(path, "rb") as w:
        return w.readframes(w.getnframes())


def generate_test_wav(path: str, duration_sec: float = 2.0):
    """Generate a simple synthetic sine-wave WAV for testing."""
    import math
    n_samples = int(SAMPLE_RATE * duration_sec)
    samples = []
    for i in range(n_samples):
        # 440 Hz sine wave at 30% amplitude
        val = int(32767 * 0.3 * math.sin(2 * math.pi * 440 * i / SAMPLE_RATE))
        samples.append(val)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(struct.pack(f"<{n_samples}h", *samples))


async def test_websocket(port: int, pcm16: bytes):
    import websockets

    uri = f"ws://127.0.0.1:{port}"
    print(f"[WS] Connecting to {uri}...")

    async with websockets.connect(uri) as ws:
        msg = await asyncio.wait_for(ws.recv(), timeout=5.0)
        data = json.loads(msg)
        assert data["type"] == "session.created"
        print(f"[WS] Session created: {data['session']['id']}")

        chunk_size = int(SAMPLE_RATE * 0.1) * 2
        for i in range(0, len(pcm16), chunk_size):
            chunk = pcm16[i:i+chunk_size]
            await ws.send(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": base64.b64encode(chunk).decode("ascii")
            }))
            await asyncio.sleep(0.05)

        await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))

        events = []
        try:
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=15.0)
                data = json.loads(msg)
                events.append(data)
                print(f"[WS] Event: {data['type']} -> {data.get('delta', data.get('transcript', ''))[:60]}")
                if data["type"] == "conversation.item.input_audio_transcription.completed":
                    break
        except asyncio.TimeoutError:
            print("[WS] Timeout waiting for completed event")

        return events


async def test_tcp(port: int, pcm16: bytes):
    """Test TCP line-delimited JSON streaming."""
    print(f"[TCP] Connecting to 127.0.0.1:{port}...")

    reader, writer = await asyncio.open_connection("127.0.0.1", port)

    chunk_size = int(SAMPLE_RATE * 0.1) * 2
    for i in range(0, len(pcm16), chunk_size):
        chunk = pcm16[i:i+chunk_size]
        msg = json.dumps({
            "type": "input_audio_buffer.append",
            "audio": base64.b64encode(chunk).decode("ascii")
        }) + "\n"
        writer.write(msg.encode("utf-8"))
        await writer.drain()
        await asyncio.sleep(0.05)

    writer.write((json.dumps({"type": "input_audio_buffer.commit"}) + "\n").encode("utf-8"))
    await writer.drain()

    events = []
    try:
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=15.0)
            if not line:
                break
            data = json.loads(line.decode("utf-8").strip())
            events.append(data)
            print(f"[TCP] Event: {data['type']} -> {data.get('delta', data.get('transcript', ''))[:60]}")
            if data["type"] == "conversation.item.input_audio_transcription.completed":
                break
    except asyncio.TimeoutError:
        print("[TCP] Timeout waiting for completed event")

    writer.close()
    await writer.wait_closed()
    return events


def test_http(http_port: int, wav_path: str):
    import urllib.request
    import io

    boundary = "----WebKitFormBoundary"
    body = io.BytesIO()
    body.write(f"--{boundary}\r\n".encode())
    body.write(b'Content-Disposition: form-data; name="file"; filename="test.wav"\r\n')
    body.write(b"Content-Type: audio/wav\r\n\r\n")
    with open(wav_path, "rb") as f:
        body.write(f.read())
    body.write(f"\r\n--{boundary}--\r\n".encode())
    data = body.getvalue()

    req = urllib.request.Request(
        f"http://127.0.0.1:{http_port}/inference",
        data=data,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(data)),
        },
        method="POST",
    )
    resp = urllib.request.urlopen(req, timeout=30)
    result = json.loads(resp.read().decode())
    print(f"[HTTP] Transcription result: {result}")
    return result


def main():
    http_port = 19080
    ws_port = 19081
    tcp_port = 19082

    # Look for a real speech sample (synthetic tones won't transcribe)
    speech_candidates = [
        "test/test_speech.wav",
    ]
    test_wav = None
    for candidate in speech_candidates:
        if os.path.exists(candidate):
            test_wav = candidate
            break

    using_temp = False
    if test_wav is None:
        print("WARNING: No real speech sample found. Creating synthetic audio.")
        print("         Moonshine may not transcribe synthetic tones.")
        print("         Place a real 16kHz mono WAV at test/test_speech.wav for full validation.")
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            test_wav = tmp.name
        generate_test_wav(test_wav, duration_sec=2.0)
        using_temp = True

    pcm16 = read_wav_pcm16(test_wav)
    print(f"Using test WAV: {test_wav} ({len(pcm16)} bytes)")

    # moonshine_voice must be installed: pip install moonshine_voice
    print("Starting moonshine-server...")
    proc = subprocess.Popen(
        [
            sys.executable,
            "tools/moonshine-server/main.py",
            "--model-arch", "5",
            "--port", str(http_port),
            "--ws-port", str(ws_port),
            "--tcp-port", str(tcp_port),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    print("Waiting for server startup...")
    time.sleep(15)

    try:
        import urllib.request
        try:
            resp = urllib.request.urlopen(f"http://127.0.0.1:{http_port}/health", timeout=5)
            print(f"[HTTP] Health check: {resp.read().decode()}")
        except Exception as e:
            print(f"[HTTP] Health check failed: {e}")

        print("\n--- Testing HTTP file transcription ---")
        http_result = test_http(http_port, test_wav)

        print("\n--- Testing WebSocket streaming ---")
        ws_events = asyncio.run(test_websocket(ws_port, pcm16))

        print("\n--- Testing TCP streaming ---")
        tcp_events = asyncio.run(test_tcp(tcp_port, pcm16))

        # Summaries
        ws_ok = any(e["type"] == "conversation.item.input_audio_transcription.completed" for e in ws_events)
        tcp_ok = any(e["type"] == "conversation.item.input_audio_transcription.completed" for e in tcp_events)
        http_ok = bool(http_result.get("text"))

        print("\n========== RESULTS ==========")
        print(f"HTTP file transcription: {'✅ PASS' if http_ok else '❌ FAIL'}")
        print(f"WebSocket streaming:     {'✅ PASS' if ws_ok else '❌ FAIL'}")
        print(f"TCP streaming:           {'✅ PASS' if tcp_ok else '❌ FAIL'}")
        print("=============================")

    finally:
        proc.terminate()
        # Clean up temp file if we created one
        if using_temp and test_wav and os.path.exists(test_wav):
            try:
                os.unlink(test_wav)
            except OSError:
                pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

        stderr = proc.stderr.read().decode() if proc.stderr else ""
        if stderr:
            print("\n--- Server stderr ---")
            print(stderr)


if __name__ == "__main__":
    main()
