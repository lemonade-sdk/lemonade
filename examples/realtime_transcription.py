"""
Realtime Audio Transcription with OpenAI SDK

Uses the official OpenAI SDK to connect to Lemonade Server's
OpenAI-compatible realtime transcription endpoint.

Requirements:
    pip install openai pyaudio websockets

Usage:
    python realtime_transcription.py
    python realtime_transcription.py --model Whisper-Small
"""

import argparse
import asyncio
import base64
import struct
import sys
import os

# Enable ANSI escape codes on Windows
if os.name == 'nt':
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        # Enable ENABLE_VIRTUAL_TERMINAL_PROCESSING
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
    except:
        pass

# Check dependencies
try:
    from openai import AsyncOpenAI
except ImportError:
    print("Error: openai library not found.")
    print("Install it with: pip install openai")
    sys.exit(1)

try:
    import pyaudio
except ImportError:
    print("Error: pyaudio library not found.")
    print("Install it with: pip install pyaudio")
    sys.exit(1)

try:
    import websockets  # noqa: F401 — required by openai SDK for realtime
except ImportError:
    print("Error: websockets library not found.")
    print("Install it with: pip install websockets")
    sys.exit(1)

TARGET_RATE = 16000  # Whisper expects 16kHz mono PCM16
CHUNK_SIZE = 4096    # Samples per read at native rate (~85ms at 48kHz)


def downsample_to_16k(pcm16_bytes, native_rate):
    """Downsample PCM16 audio from native_rate to 16kHz using linear interpolation.

    Matches the resampling approach used in the Electron app's useAudioCapture hook.
    """
    if native_rate == TARGET_RATE:
        return pcm16_bytes

    n_samples = len(pcm16_bytes) // 2
    samples = struct.unpack(f'<{n_samples}h', pcm16_bytes)

    ratio = native_rate / TARGET_RATE
    output_length = int(n_samples / ratio)
    output = bytearray(output_length * 2)

    for i in range(output_length):
        src_idx = i * ratio
        idx_floor = int(src_idx)
        idx_ceil = min(idx_floor + 1, n_samples - 1)
        frac = src_idx - idx_floor
        sample = samples[idx_floor] * (1 - frac) + samples[idx_ceil] * frac
        clamped = max(-32768, min(32767, int(sample)))
        struct.pack_into('<h', output, i * 2, clamped)

    return bytes(output)


def transcribe_microphone(model: str, server_url: str):
    """Stream microphone audio using OpenAI SDK's realtime API."""
    import urllib.request
    import json

    # Load model via REST API first
    print(f"Loading model: {model}...")
    try:
        req = urllib.request.Request(
            f"{server_url}/load",
            data=json.dumps({"model_name": model}).encode(),
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            print(f"Model loaded: {model}")
    except Exception as e:
        print(f"Error loading model: {e}")
        print("Make sure Lemonade Server is running: lemonade-server serve")
        return

    # Get WebSocket port from /health endpoint
    try:
        health_url = server_url + "/health"
        with urllib.request.urlopen(health_url, timeout=10) as resp:
            health = json.loads(resp.read().decode())
            ws_port = health.get("websocket_port")
            if not ws_port:
                print("Error: Server did not provide websocket_port in /health response")
                return
            print(f"WebSocket port: {ws_port}")
    except Exception as e:
        print(f"Error fetching WebSocket port: {e}")
        return

    # Create OpenAI client pointing at local server
    client = AsyncOpenAI(
        api_key="unused",
        base_url=server_url,
        websocket_base_url=f"ws://localhost:{ws_port}",
    )

    async def run():
        print("Connecting to realtime endpoint...")

        async with client.beta.realtime.connect(model=model) as conn:
            # Wait for session.created
            event = await asyncio.wait_for(conn.recv(), timeout=10)
            print(f"Session: {event.session.id}")

            # Initialize microphone at its native sample rate
            pa = pyaudio.PyAudio()
            device_info = pa.get_default_input_device_info()
            native_rate = int(device_info['defaultSampleRate'])
            print(f"Microphone native sample rate: {native_rate} Hz")

            stream = pa.open(
                format=pyaudio.paInt16,
                channels=1,
                rate=native_rate,
                input=True,
                frames_per_buffer=CHUNK_SIZE
            )

            print("Recording... Press Ctrl+C to stop")
            print("-" * 40)

            transcripts = []

            async def send_audio():
                try:
                    while True:
                        data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                        # Downsample from native rate to 16kHz (matching Electron app)
                        data = downsample_to_16k(data, native_rate)
                        await conn.input_audio_buffer.append(
                            audio=base64.b64encode(data).decode()
                        )
                        await asyncio.sleep(0.01)
                except asyncio.CancelledError:
                    pass

            async def receive_messages():
                nonlocal transcripts
                # Get terminal width to avoid line-wrapping issues with \r
                try:
                    term_width = os.get_terminal_size().columns
                except OSError:
                    term_width = 80
                try:
                    async for event in conn:
                        if event.type == "conversation.item.input_audio_transcription.delta":
                            delta_text = getattr(event, "delta", "").replace('\n', ' ').strip()
                            if delta_text:
                                # Truncate to one terminal line so \r can fully overwrite
                                if len(delta_text) > term_width - 4:
                                    delta_text = "..." + delta_text[-(term_width - 4):]
                                print(f"\r\033[2K{delta_text}", end="", flush=True)
                        elif event.type == "conversation.item.input_audio_transcription.completed":
                            transcript = getattr(event, "transcript", "").replace('\n', ' ').strip()
                            if transcript:
                                transcripts.append(transcript)
                                # Clear interim line, print final on its own line
                                print(f"\r\033[2K{transcript}")
                        elif event.type == "error":
                            error = getattr(event, "error", None)
                            msg = getattr(error, "message", "Unknown") if error else "Unknown"
                            print(f"\nError: {msg}")
                except asyncio.CancelledError:
                    pass

            send_task = asyncio.create_task(send_audio())
            recv_task = asyncio.create_task(receive_messages())

            try:
                await asyncio.gather(send_task, recv_task)
            except KeyboardInterrupt:
                print("\n\nStopping...")
                send_task.cancel()
                recv_task.cancel()

                # Commit remaining audio
                await conn.input_audio_buffer.commit()

                # Wait for final transcript
                try:
                    while True:
                        event = await asyncio.wait_for(conn.recv(), timeout=3)
                        if event.type == "conversation.item.input_audio_transcription.completed":
                            transcript = getattr(event, "transcript", "").strip()
                            if transcript:
                                transcripts.append(transcript)
                                print(f">>> {transcript}")
                            break
                except:
                    pass

            finally:
                stream.stop_stream()
                stream.close()
                pa.terminate()

            if transcripts:
                print("\n" + "-" * 40)
                print("Full transcript:")
                print(" ".join(transcripts))

    asyncio.run(run())


def main():
    parser = argparse.ArgumentParser(
        description="Realtime transcription using OpenAI-compatible API"
    )
    parser.add_argument(
        "--model",
        default="Whisper-Tiny",
        help="Whisper model (default: Whisper-Tiny)"
    )
    parser.add_argument(
        "--server",
        default="http://localhost:8000/api/v1",
        help="REST API URL"
    )

    args = parser.parse_args()
    transcribe_microphone(args.model, args.server)


if __name__ == "__main__":
    main()
