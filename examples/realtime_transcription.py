"""
Realtime Audio Transcription Example

Stream microphone audio to the Lemonade Server WebSocket endpoint
for real-time transcription with Voice Activity Detection (VAD).

Requirements:
    pip install websockets pyaudio

Usage:
    # Stream from microphone (the primary realtime use case)
    python realtime_transcription.py --mic

    # Specify model
    python realtime_transcription.py --mic --model Whisper-Small

    # Test mode: simulate streaming with a WAV file (for development/testing)
    python realtime_transcription.py --file audio.wav
"""

import argparse
import asyncio
import base64
import json
import os
import struct
import sys
import urllib.request
import urllib.error
import wave

try:
    import websockets
    from websockets.exceptions import ConnectionClosedError, InvalidURI, InvalidHandshake
except ImportError:
    print("Error: websockets library not found.")
    print("Install it with: pip install websockets")
    sys.exit(1)

SAMPLE_RATE = 16000
CHUNK_SIZE = 4096  # ~256ms at 16kHz
SERVER_URL = "http://localhost:8000"
WS_URL = "ws://localhost:8100/api/v1/realtime?intent=transcription"


def check_server(server_url: str = SERVER_URL) -> bool:
    """Check if Lemonade Server is running."""
    try:
        url = f"{server_url}/api/v1/health"
        with urllib.request.urlopen(url, timeout=5) as resp:
            return resp.status == 200
    except urllib.error.URLError:
        return False
    except Exception:
        return False


def load_model(model: str, server_url: str = SERVER_URL) -> bool:
    """Load a Whisper model on the server via REST API."""
    print(f"Loading model: {model}...")

    url = f"{server_url}/api/v1/load"
    data = json.dumps({"model_name": model}).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})

    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            result = json.loads(resp.read().decode())
            print(f"Model loaded: {result.get('model_name', model)}")
            return True
    except urllib.error.HTTPError as e:
        error_body = e.read().decode() if e.fp else ""
        try:
            error_json = json.loads(error_body)
            error_msg = error_json.get("error", {}).get("message", error_body)
        except json.JSONDecodeError:
            error_msg = error_body

        if e.code == 404:
            print(f"Error: Model '{model}' not found.")
            print("Available Whisper models: Whisper-Tiny, Whisper-Small, Whisper-Medium")
        else:
            print(f"Error loading model: {error_msg}")
        return False
    except urllib.error.URLError as e:
        print(f"Error: Cannot connect to server at {server_url}")
        print(f"Reason: {e.reason}")
        print("\nMake sure Lemonade Server is running:")
        print("  lemonade-server serve")
        return False
    except TimeoutError:
        print("Error: Timeout while loading model (this can happen for large models on first load)")
        print("The model may still be downloading. Try again in a few minutes.")
        return False


async def transcribe_wav_file(filepath: str, model: str = "Whisper-Tiny",
                               ws_url: str = WS_URL, server_url: str = SERVER_URL):
    """Test mode: simulate realtime streaming using a WAV file."""

    # Validate file exists
    if not os.path.exists(filepath):
        print(f"Error: File not found: {filepath}")
        return

    if not filepath.lower().endswith('.wav'):
        print(f"Error: File must be a WAV file: {filepath}")
        return

    # Check server is running
    if not check_server(server_url):
        print(f"Error: Cannot connect to Lemonade Server at {server_url}")
        print("\nMake sure the server is running:")
        print("  lemonade-server serve")
        return

    # Load the model first
    if not load_model(model, server_url):
        return

    print(f"Transcribing: {filepath}")
    print("-" * 40)

    try:
        async with websockets.connect(ws_url) as ws:
            # Wait for session created
            msg = json.loads(await ws.recv())
            print(f"Session: {msg['session']['id']}")

            # Configure model
            await ws.send(json.dumps({
                "type": "transcription_session.update",
                "session": {"model": model}
            }))
            await ws.recv()  # session.updated

            # Read and send WAV file in chunks
            try:
                with wave.open(filepath, 'rb') as wav:
                    sample_rate = wav.getframerate()
                    n_channels = wav.getnchannels()
                    sampwidth = wav.getsampwidth()
                    chunk_samples = int(sample_rate * 0.1)  # 100ms chunks

                    print(f"Audio: {sample_rate}Hz, {n_channels}ch, {sampwidth*8}bit")

                    while True:
                        frames = wav.readframes(chunk_samples)
                        if not frames:
                            break

                        # Convert to mono if stereo
                        if n_channels == 2:
                            samples = struct.unpack(f'<{len(frames)//2}h', frames)
                            samples = [(samples[i] + samples[i+1]) // 2
                                       for i in range(0, len(samples), 2)]
                            frames = struct.pack(f'<{len(samples)}h', *samples)

                        # Resample to 16kHz if needed
                        if sample_rate != 16000:
                            samples = struct.unpack(f'<{len(frames)//2}h', frames)
                            ratio = sample_rate / 16000
                            samples = [samples[min(int(i * ratio), len(samples)-1)]
                                       for i in range(int(len(samples) / ratio))]
                            frames = struct.pack(f'<{len(samples)}h', *samples)

                        # Send base64-encoded audio
                        await ws.send(json.dumps({
                            "type": "input_audio_buffer.append",
                            "audio": base64.b64encode(frames).decode()
                        }))
                        await asyncio.sleep(0.05)  # Simulate real-time

            except wave.Error as e:
                print(f"Error reading WAV file: {e}")
                return

            # Commit and wait for transcription
            print("Processing...")
            await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))

            # Collect transcriptions
            while True:
                try:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
                    if msg["type"] == "conversation.item.input_audio_transcription.completed":
                        print(f"\nTranscript: {msg['transcript']}")
                        break
                    elif msg["type"] == "input_audio_buffer.speech_started":
                        print("  [speech detected]")
                    elif msg["type"] == "input_audio_buffer.speech_stopped":
                        print("  [speech ended]")
                    elif msg["type"] == "error":
                        print(f"Error from server: {msg['error']['message']}")
                        break
                except asyncio.TimeoutError:
                    print("Timeout waiting for transcription")
                    break

    except ConnectionRefusedError:
        print(f"Error: Cannot connect to WebSocket server at {ws_url}")
        print("Make sure Lemonade Server is running with WebSocket support enabled.")
    except InvalidURI:
        print(f"Error: Invalid WebSocket URL: {ws_url}")
    except InvalidHandshake as e:
        print(f"Error: WebSocket handshake failed: {e}")
    except ConnectionClosedError as e:
        print(f"Error: WebSocket connection closed unexpectedly: {e}")


async def transcribe_microphone(model: str = "Whisper-Tiny",
                                 ws_url: str = WS_URL, server_url: str = SERVER_URL):
    """Stream microphone audio to the realtime transcription endpoint."""

    # Check for pyaudio
    try:
        import pyaudio
    except ImportError:
        print("Error: pyaudio library not found.")
        print("Install it with: pip install pyaudio")
        print("\nOn Windows, you may need to install from wheel:")
        print("  pip install pipwin && pipwin install pyaudio")
        return

    # Check server is running
    if not check_server(server_url):
        print(f"Error: Cannot connect to Lemonade Server at {server_url}")
        print("\nMake sure the server is running:")
        print("  lemonade-server serve")
        return

    # Load the model first
    if not load_model(model, server_url):
        return

    # Initialize PyAudio and check for microphone
    pa = pyaudio.PyAudio()

    # Check if any input devices are available
    input_device_count = sum(1 for i in range(pa.get_device_count())
                             if pa.get_device_info_by_index(i)['maxInputChannels'] > 0)
    if input_device_count == 0:
        print("Error: No microphone found.")
        print("Make sure a microphone is connected and enabled.")
        pa.terminate()
        return

    try:
        stream = pa.open(
            format=pyaudio.paInt16,
            channels=1,
            rate=SAMPLE_RATE,
            input=True,
            frames_per_buffer=CHUNK_SIZE
        )
    except OSError as e:
        print(f"Error opening microphone: {e}")
        print("Make sure your microphone is not in use by another application.")
        pa.terminate()
        return

    print("Recording... Press Ctrl+C to stop")
    print("-" * 40)

    try:
        async with websockets.connect(ws_url) as ws:
            # Wait for session
            msg = json.loads(await ws.recv())
            print(f"Session: {msg['session']['id']}")

            # Configure model
            await ws.send(json.dumps({
                "type": "transcription_session.update",
                "session": {"model": model}
            }))
            await ws.recv()

            transcripts = []

            async def send_audio():
                """Continuously send audio chunks."""
                try:
                    while True:
                        try:
                            data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                            await ws.send(json.dumps({
                                "type": "input_audio_buffer.append",
                                "audio": base64.b64encode(data).decode()
                            }))
                            await asyncio.sleep(0.01)
                        except OSError as e:
                            print(f"\nMicrophone error: {e}")
                            break
                except asyncio.CancelledError:
                    pass

            async def receive_messages():
                """Handle incoming messages."""
                try:
                    while True:
                        msg = json.loads(await ws.recv())
                        msg_type = msg["type"]

                        if msg_type == "conversation.item.input_audio_transcription.completed":
                            transcript = msg['transcript'].strip()
                            if transcript:
                                transcripts.append(transcript)
                                print(f"\n>>> {transcript}")
                        elif msg_type == "input_audio_buffer.speech_started":
                            print("\r[listening...]", end="", flush=True)
                        elif msg_type == "input_audio_buffer.speech_stopped":
                            print("\r[transcribing...]", end="", flush=True)
                        elif msg_type == "error":
                            print(f"\nError from server: {msg['error']['message']}")
                except asyncio.CancelledError:
                    pass
                except ConnectionClosedError:
                    print("\nConnection to server lost.")

            send_task = asyncio.create_task(send_audio())
            recv_task = asyncio.create_task(receive_messages())

            try:
                await asyncio.gather(send_task, recv_task)
            except KeyboardInterrupt:
                print("\n\nStopping...")
                send_task.cancel()
                recv_task.cancel()

                # Commit any remaining audio
                try:
                    await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))

                    # Wait for final transcript
                    while True:
                        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3))
                        if msg["type"] == "conversation.item.input_audio_transcription.completed":
                            transcript = msg['transcript'].strip()
                            if transcript:
                                transcripts.append(transcript)
                                print(f">>> {transcript}")
                            break
                except (asyncio.TimeoutError, ConnectionClosedError):
                    pass

            if transcripts:
                print("\n" + "-" * 40)
                print("Full transcript:")
                print(" ".join(transcripts))

    except ConnectionRefusedError:
        print(f"Error: Cannot connect to WebSocket server at {ws_url}")
        print("Make sure Lemonade Server is running with WebSocket support enabled.")
    except InvalidURI:
        print(f"Error: Invalid WebSocket URL: {ws_url}")
    except InvalidHandshake as e:
        print(f"Error: WebSocket handshake failed: {e}")
    except ConnectionClosedError as e:
        print(f"Error: WebSocket connection closed unexpectedly: {e}")
    finally:
        stream.stop_stream()
        stream.close()
        pa.terminate()


def main():
    parser = argparse.ArgumentParser(
        description="Realtime audio transcription with Lemonade Server"
    )
    parser.add_argument(
        "--mic", "-m",
        action="store_true",
        help="Stream from microphone (primary realtime mode)"
    )
    parser.add_argument(
        "--file", "-f",
        help="Test mode: simulate streaming with a WAV file"
    )
    parser.add_argument(
        "--model",
        default="Whisper-Tiny",
        help="Whisper model to use (default: Whisper-Tiny)"
    )
    parser.add_argument(
        "--server",
        default=SERVER_URL,
        help=f"Server URL for REST API (default: {SERVER_URL})"
    )
    parser.add_argument(
        "--ws-url",
        default=WS_URL,
        help=f"WebSocket URL (default: {WS_URL})"
    )

    args = parser.parse_args()

    if args.file:
        asyncio.run(transcribe_wav_file(args.file, args.model, args.ws_url, args.server))
    elif args.mic:
        asyncio.run(transcribe_microphone(args.model, args.ws_url, args.server))
    else:
        parser.print_help()
        print("\nExamples:")
        print("  python realtime_transcription.py --mic")
        print("  python realtime_transcription.py --mic --model Whisper-Small")
        print("  python realtime_transcription.py --file audio.wav")


if __name__ == "__main__":
    main()
