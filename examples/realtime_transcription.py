"""
Realtime Audio Transcription with OpenAI SDK

Uses the official OpenAI SDK to connect to Lemonade Server's
OpenAI-compatible realtime transcription endpoint.

Requirements:
    pip install openai pyaudio

Usage:
    python realtime_transcription.py --mic
    python realtime_transcription.py --mic --model Whisper-Small
"""

import argparse
import base64
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
    from openai import OpenAI
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

SAMPLE_RATE = 16000
CHUNK_SIZE = 4096


def transcribe_microphone(model: str, server_url: str):
    """Stream microphone audio using OpenAI SDK."""
    import urllib.request
    import json

    # Load model via REST API first
    print(f"Loading model: {model}...")
    client = OpenAI(base_url=server_url, api_key="unused")

    try:
        # Use the models endpoint to trigger model loading
        # The /load endpoint expects model_name
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
        health_url = server_url.replace("/api/v1", "") + "/api/v1/health"
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

    # Connect to WebSocket using openai's low-level websocket
    # Note: OpenAI SDK expects wss:// but we use ws:// for local
    print("Connecting to realtime endpoint...")

    try:
        import websockets
        import asyncio
    except ImportError:
        print("Error: websockets library not found.")
        print("Install it with: pip install websockets")
        return

    async def run():
        url = f"ws://localhost:{ws_port}/realtime?model={model}"
        print(f"WebSocket URL: {url}")

        async with websockets.connect(url) as ws:
            # Wait for session.created
            import json
            msg = json.loads(await ws.recv())
            print(f"Session: {msg['session']['id']}")

            # Initialize microphone
            pa = pyaudio.PyAudio()
            stream = pa.open(
                format=pyaudio.paInt16,
                channels=1,
                rate=SAMPLE_RATE,
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
                        await ws.send(json.dumps({
                            "type": "input_audio_buffer.append",
                            "audio": base64.b64encode(data).decode()
                        }))
                        await asyncio.sleep(0.01)
                except asyncio.CancelledError:
                    pass

            async def receive_messages():
                nonlocal transcripts

                # Simple approach:
                # - Delta: update current line in-place (just the current utterance)
                # - Completed: print on new line and move on

                try:
                    while True:
                        msg = json.loads(await ws.recv())
                        msg_type = msg.get("type", "")

                        if msg_type == "conversation.item.input_audio_transcription.delta":
                            # Interim: show just this delta, update in place
                            delta_text = msg.get("delta", "").replace('\n', ' ').strip()
                            if delta_text:
                                # Clear line and print (works on Windows with VT mode)
                                print(f"\r{delta_text}\033[K", end="", flush=True)
                        elif msg_type == "conversation.item.input_audio_transcription.completed":
                            # Final: print and move to new line
                            transcript = msg.get("transcript", "").replace('\n', ' ').strip()
                            if transcript:
                                transcripts.append(transcript)
                                print(f"\r{transcript}\033[K")  # newline at end
                        elif msg_type == "error":
                            print(f"\nError: {msg.get('error', {}).get('message', 'Unknown')}")
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
                await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))

                # Wait for final transcript
                try:
                    while True:
                        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=3))
                        if msg.get("type") == "conversation.item.input_audio_transcription.completed":
                            transcript = msg.get("transcript", "").strip()
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
        "--mic", "-m",
        action="store_true",
        help="Stream from microphone"
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

    if args.mic:
        transcribe_microphone(args.model, args.server)
    else:
        parser.print_help()
        print("\nExample:")
        print("  python realtime_transcription.py --mic")
        print("  python realtime_transcription.py --mic --model Whisper-Small")


if __name__ == "__main__":
    main()
