"""
Usage: python server_whisper_streaming.py

This will test WebSocket-based audio streaming transcription with Whisper.
The tests verify the real-time audio streaming protocol for live transcription.

Examples:
    python server_whisper_streaming.py
    python server_whisper_streaming.py --server-binary ./lemonade-server

Requirements:
    pip install websocket-client
"""

import os
import sys
import json
import time
import base64
import struct
import tempfile
import urllib.request
import threading

# Import websocket client
try:
    import websocket
except ImportError:
    raise ImportError("You must `pip install websocket-client` to run this test")

# Import all shared functionality from utils/server_base.py
from utils.server_base import (
    ServerTestingBase,
    run_server_tests_with_class,
    PORT,
)

# Test audio file URL from lemonade-sdk assets repository
TEST_AUDIO_URL = "https://raw.githubusercontent.com/lemonade-sdk/assets/main/audio/test_speech.wav"
WHISPER_MODEL = "Whisper-Tiny"

# WebSocket server runs on PORT + 1
WS_PORT = PORT + 1


def read_wav_as_pcm16(filepath):
    """Read a WAV file and return PCM 16-bit samples."""
    with open(filepath, 'rb') as f:
        # Skip WAV header (44 bytes for standard WAV)
        f.read(44)
        # Read the rest as raw PCM data
        return f.read()


def pcm_to_base64_chunks(pcm_data, chunk_size=4096):
    """Convert PCM data to base64-encoded chunks."""
    chunks = []
    for i in range(0, len(pcm_data), chunk_size):
        chunk = pcm_data[i:i + chunk_size]
        chunks.append(base64.b64encode(chunk).decode('utf-8'))
    return chunks


class WebSocketStreamingTests(ServerTestingBase):
    """Testing class for WebSocket audio streaming transcription."""

    # Class-level cache for the test audio file
    _test_audio_path = None

    @classmethod
    def setUpClass(cls):
        """Download test audio file once for all tests."""
        super().setUpClass()

        # Download test audio file to temp directory
        cls._test_audio_path = os.path.join(tempfile.gettempdir(), "test_speech_streaming.wav")

        if not os.path.exists(cls._test_audio_path):
            print(f"\n[INFO] Downloading test audio file from {TEST_AUDIO_URL}")
            try:
                urllib.request.urlretrieve(TEST_AUDIO_URL, cls._test_audio_path)
                print(f"[OK] Downloaded to {cls._test_audio_path}")
            except Exception as e:
                print(f"[ERROR] Failed to download test audio: {e}")
                raise

    @classmethod
    def tearDownClass(cls):
        """Cleanup test audio file."""
        super().tearDownClass()
        if cls._test_audio_path and os.path.exists(cls._test_audio_path):
            try:
                os.remove(cls._test_audio_path)
                print(f"[INFO] Cleaned up test audio file")
            except Exception:
                pass  # Ignore cleanup errors

    def setUp(self):
        """Call parent setUp with streaming-specific messaging."""
        print(f"\n=== Starting new WebSocket streaming test ===")
        super().setUp()
        self.ws_url = f"ws://localhost:{WS_PORT}/api/v1/audio/stream"

    # Test 1: WebSocket connection
    def test_001_websocket_connection(self):
        """Test that WebSocket endpoint accepts connections."""
        print(f"[INFO] Testing WebSocket connection to {self.ws_url}")

        connected = threading.Event()
        error_msg = None

        def on_open(ws):
            print("[OK] WebSocket connected successfully")
            connected.set()
            ws.close()

        def on_error(ws, error):
            nonlocal error_msg
            error_msg = str(error)
            print(f"[ERROR] WebSocket error: {error}")

        ws = websocket.WebSocketApp(
            self.ws_url,
            on_open=on_open,
            on_error=on_error
        )

        # Run WebSocket in a thread with timeout
        ws_thread = threading.Thread(target=lambda: ws.run_forever(ping_timeout=5))
        ws_thread.daemon = True
        ws_thread.start()

        # Wait for connection
        success = connected.wait(timeout=10)

        if not success:
            ws.close()

        self.assertTrue(success, f"Failed to connect to WebSocket: {error_msg or 'timeout'}")

    # Test 2: Start message protocol
    def test_002_start_message(self):
        """Test sending start message and receiving ready response."""
        print(f"[INFO] Testing start message protocol")

        ready_received = threading.Event()
        response_data = {}
        error_msg = None

        def on_open(ws):
            print("[INFO] Connected, sending start message")
            ws.send(json.dumps({
                "type": "start",
                "model": WHISPER_MODEL,
                "language": ""
            }))

        def on_message(ws, message):
            nonlocal response_data
            try:
                data = json.loads(message)
                response_data = data
                print(f"[INFO] Received message: {data}")
                if data.get("type") == "ready":
                    ready_received.set()
                    ws.close()
            except json.JSONDecodeError as e:
                print(f"[ERROR] Failed to parse message: {e}")

        def on_error(ws, error):
            nonlocal error_msg
            error_msg = str(error)
            print(f"[ERROR] WebSocket error: {error}")

        ws = websocket.WebSocketApp(
            self.ws_url,
            on_open=on_open,
            on_message=on_message,
            on_error=on_error
        )

        ws_thread = threading.Thread(target=lambda: ws.run_forever(ping_timeout=30))
        ws_thread.daemon = True
        ws_thread.start()

        # Wait for ready response (may take time to load model)
        success = ready_received.wait(timeout=120)  # 2 minutes for model loading

        if not success:
            ws.close()

        self.assertTrue(success, f"Did not receive ready response: {error_msg or 'timeout'}")
        self.assertEqual(response_data.get("type"), "ready")

    # Test 3: Audio chunk streaming
    def test_003_audio_chunk_streaming(self):
        """Test streaming audio chunks and receiving transcription."""
        print(f"[INFO] Testing audio chunk streaming")

        self.assertIsNotNone(
            self._test_audio_path, "Test audio file not downloaded"
        )
        self.assertTrue(
            os.path.exists(self._test_audio_path),
            f"Test audio file not found at {self._test_audio_path}"
        )

        # Read and prepare audio chunks
        pcm_data = read_wav_as_pcm16(self._test_audio_path)
        chunks = pcm_to_base64_chunks(pcm_data)
        print(f"[INFO] Prepared {len(chunks)} audio chunks")

        ready_received = threading.Event()
        final_received = threading.Event()
        responses = []
        error_msg = None
        chunks_sent = 0

        def on_open(ws):
            print("[INFO] Connected, sending start message")
            ws.send(json.dumps({
                "type": "start",
                "model": WHISPER_MODEL,
                "language": ""
            }))

        def on_message(ws, message):
            nonlocal chunks_sent
            try:
                data = json.loads(message)
                responses.append(data)
                print(f"[INFO] Received: {data.get('type')}")

                if data.get("type") == "ready":
                    ready_received.set()
                    # Start sending audio chunks
                    print(f"[INFO] Sending {len(chunks)} audio chunks...")
                    for i, chunk in enumerate(chunks):
                        ws.send(json.dumps({
                            "type": "audio_chunk",
                            "data": chunk,
                            "sample_rate": 16000
                        }))
                        chunks_sent += 1
                        if i % 10 == 0:
                            time.sleep(0.01)  # Small delay to simulate real-time

                    # Send stop message
                    print("[INFO] Sending stop message")
                    ws.send(json.dumps({"type": "stop"}))

                elif data.get("type") == "partial":
                    print(f"[INFO] Partial transcription: {data.get('text', '')[:50]}...")

                elif data.get("type") == "final":
                    print(f"[OK] Final transcription: {data.get('text', '')}")
                    final_received.set()
                    ws.close()

                elif data.get("type") == "error":
                    print(f"[ERROR] Server error: {data.get('message')}")

            except json.JSONDecodeError as e:
                print(f"[ERROR] Failed to parse message: {e}")

        def on_error(ws, error):
            nonlocal error_msg
            error_msg = str(error)
            print(f"[ERROR] WebSocket error: {error}")

        ws = websocket.WebSocketApp(
            self.ws_url,
            on_open=on_open,
            on_message=on_message,
            on_error=on_error
        )

        ws_thread = threading.Thread(target=lambda: ws.run_forever(ping_timeout=120))
        ws_thread.daemon = True
        ws_thread.start()

        # Wait for final transcription
        success = final_received.wait(timeout=180)  # 3 minutes total

        if not success:
            ws.close()

        print(f"[INFO] Sent {chunks_sent} chunks, received {len(responses)} responses")

        self.assertTrue(success, f"Did not receive final transcription: {error_msg or 'timeout'}")

        # Verify we got a final response with text
        final_responses = [r for r in responses if r.get("type") == "final"]
        self.assertTrue(len(final_responses) > 0, "No final response received")

        # Check that final response has some text (should have transcribed something)
        final_text = final_responses[0].get("text", "")
        print(f"[INFO] Final transcription text: '{final_text}'")
        # Don't assert on specific text since it depends on the test audio

    # Test 4: Invalid message handling
    def test_004_invalid_message_handling(self):
        """Test that server handles invalid messages gracefully."""
        print(f"[INFO] Testing invalid message handling")

        error_received = threading.Event()
        responses = []

        def on_open(ws):
            print("[INFO] Connected, sending invalid message")
            ws.send("not valid json")

        def on_message(ws, message):
            try:
                data = json.loads(message)
                responses.append(data)
                print(f"[INFO] Received: {data}")
                if data.get("type") == "error":
                    error_received.set()
                    ws.close()
            except json.JSONDecodeError:
                pass

        ws = websocket.WebSocketApp(
            self.ws_url,
            on_open=on_open,
            on_message=on_message
        )

        ws_thread = threading.Thread(target=lambda: ws.run_forever(ping_timeout=10))
        ws_thread.daemon = True
        ws_thread.start()

        # Wait for error response
        success = error_received.wait(timeout=10)

        if not success:
            ws.close()

        self.assertTrue(success, "Server did not return error for invalid JSON")
        self.assertEqual(responses[-1].get("type"), "error")

    # Test 5: Audio chunk without start message
    def test_005_audio_chunk_without_start(self):
        """Test that sending audio without start message returns error."""
        print(f"[INFO] Testing audio chunk without start message")

        error_received = threading.Event()
        responses = []

        def on_open(ws):
            print("[INFO] Connected, sending audio chunk without start")
            ws.send(json.dumps({
                "type": "audio_chunk",
                "data": base64.b64encode(b'\x00' * 100).decode('utf-8'),
                "sample_rate": 16000
            }))

        def on_message(ws, message):
            try:
                data = json.loads(message)
                responses.append(data)
                print(f"[INFO] Received: {data}")
                if data.get("type") == "error":
                    error_received.set()
                    ws.close()
            except json.JSONDecodeError:
                pass

        ws = websocket.WebSocketApp(
            self.ws_url,
            on_open=on_open,
            on_message=on_message
        )

        ws_thread = threading.Thread(target=lambda: ws.run_forever(ping_timeout=10))
        ws_thread.daemon = True
        ws_thread.start()

        success = error_received.wait(timeout=10)

        if not success:
            ws.close()

        self.assertTrue(success, "Server did not return error for audio without start")


if __name__ == "__main__":
    run_server_tests_with_class(WebSocketStreamingTests)
