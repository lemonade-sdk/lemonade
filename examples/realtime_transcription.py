"""
Realtime Audio Transcription Examples

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
import struct
import urllib.request
import urllib.error
import wave

try:
    import websockets
except ImportError:
    print("Please install websockets: pip install websockets")
    exit(1)

SAMPLE_RATE = 16000
CHUNK_SIZE = 4096  # ~256ms at 16kHz
SERVER_URL = "http://localhost:8000"
WS_URL = "ws://localhost:8100/api/v1/realtime?intent=transcription"


def load_model(model: str, server_url: str = SERVER_URL):
    """Load a Whisper model on the server via REST API."""
    print(f"Loading model: {model}...")

    url = f"{server_url}/api/v1/load"
    data = json.dumps({"model_name": model}).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})

    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            result = json.loads(resp.read().decode())
            print(f"Model loaded: {result.get('model', model)}")
            return True
    except urllib.error.HTTPError as e:
        error_body = e.read().decode() if e.fp else ""
        print(f"Failed to load model: {e.code} - {error_body}")
        return False
    except urllib.error.URLError as e:
        print(f"Cannot connect to server at {server_url}: {e.reason}")
        print("Make sure Lemonade Server is running.")
        return False


async def transcribe_wav_file(filepath: str, model: str = "Whisper-Tiny", ws_url: str = WS_URL, server_url: str = SERVER_URL):
    """Test mode: simulate realtime streaming using a WAV file."""
    # Load the model first
    if not load_model(model, server_url):
        return

    print(f"Transcribing: {filepath}")
    print("-" * 40)

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
                    print(f"Error: {msg['error']['message']}")
                    break
            except asyncio.TimeoutError:
                print("Timeout waiting for transcription")
                break


async def transcribe_microphone(model: str = "Whisper-Tiny", ws_url: str = WS_URL, server_url: str = SERVER_URL):
    """Stream microphone audio to the realtime transcription endpoint."""
    try:
        import pyaudio
    except ImportError:
        print("Please install pyaudio: pip install pyaudio")
        return

    # Load the model first
    if not load_model(model, server_url):
        return

    print("Recording... Press Ctrl+C to stop")
    print("-" * 40)

    # Initialize PyAudio
    pa = pyaudio.PyAudio()
    stream = pa.open(
        format=pyaudio.paInt16,
        channels=1,
        rate=SAMPLE_RATE,
        input=True,
        frames_per_buffer=CHUNK_SIZE
    )

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
                    data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                    await ws.send(json.dumps({
                        "type": "input_audio_buffer.append",
                        "audio": base64.b64encode(data).decode()
                    }))
                    await asyncio.sleep(0.01)
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
                        print(f"\nError: {msg['error']['message']}")
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

            # Commit any remaining audio
            await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))

            # Wait for final transcript
            try:
                while True:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3))
                    if msg["type"] == "conversation.item.input_audio_transcription.completed":
                        transcript = msg['transcript'].strip()
                        if transcript:
                            transcripts.append(transcript)
                            print(f">>> {transcript}")
                        break
            except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
                pass

        finally:
            stream.stop_stream()
            stream.close()
            pa.terminate()

        if transcripts:
            print("\n" + "-" * 40)
            print("Full transcript:")
            print(" ".join(transcripts))


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
        print("  python realtime_transcription.py --file audio.wav")
        print("  python realtime_transcription.py --mic")
        print("  python realtime_transcription.py --mic --model Whisper-Small")


if __name__ == "__main__":
    main()
