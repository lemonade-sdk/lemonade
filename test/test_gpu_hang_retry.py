#!/usr/bin/env python3
"""
Integration test for lemond GPU Hang / Compute Error recovery.
Verifies that when a backend returns status 500 with a "Compute error."
or "GPU Hang" message, lemond automatically triggers a watchdog reset,
evicts/terminates the corrupted subprocess, reloads the model, and
retries the request transparently.
"""

import os
import sys
import json
import time
import shutil
import subprocess
import requests
import unittest
import concurrent.futures

# Add test/ to path
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from utils.test_models import ENDPOINT_TEST_MODEL, get_default_lemond_binary
from utils.server_base import parse_args, get_cli_binary, run_server_tests

args = parse_args()

PORT = 13333
BASE_URL = f"http://127.0.0.1:{PORT}"
MOCK_BIN_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "build", "mock-bin"
)
STATE_FILE = os.path.join(MOCK_BIN_DIR, "state.json")

MOCK_LLAMA_SERVER = """#!/usr/bin/env python3
import sys
import os
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

state_file = {state_file_repr}
attempt = 1
state = {{}}
if os.path.exists(state_file):
    try:
        with open(state_file, "r") as f:
            state = json.load(f)
            attempt = state.get("attempt", 0) + 1
    except Exception as e:
        print("Failed to read state:", e)

state["attempt"] = attempt
try:
    with open(state_file, "w") as f:
        json.dump(state, f)
except Exception as e:
    print("Failed to write state:", e)

# Parse command line args to find the port
port = 8080
for i in range(len(sys.argv)):
    if sys.argv[i] == "--port" and i + 1 < len(sys.argv):
        port = int(sys.argv[i+1])

print(f"Mock llama-server attempt {{attempt}} starting on port {{port}}")

barrier = threading.Barrier(2)
state_lock = threading.Lock()

class MockLlamaServer(BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)

        if attempt == 1:
            with state_lock:
                try:
                    if os.path.exists(state_file):
                        with open(state_file, "r") as f:
                            state = json.load(f)
                    else:
                        state = {{}}
                    state["attempt_1_requests"] = state.get("attempt_1_requests", 0) + 1
                    with open(state_file, "w") as f:
                        json.dump(state, f)
                except Exception as e:
                    print("Failed to update state with request count:", e)

            if b"concurrent-test" in body:
                # Synchronize concurrent requests on the first attempt
                try:
                    barrier.wait(timeout=2.0)
                except threading.BrokenBarrierError:
                    pass

            # First attempt: return GPU hang / compute error
            resp = {{"error": {{"code": 500, "message": "Compute error.", "type": "server_error"}}}}
            payload = json.dumps(resp).encode()
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        else:
            # Subsequent attempts: return success
            if "stream" in self.path or b'"stream":true' in body:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()

                chunks = [
                    {{"choices": [{{"delta": {{"content": "Mock "}}}}]}},
                    {{"choices": [{{"delta": {{"content": "stream "}}}}]}},
                    {{"choices": [{{"delta": {{"content": "success!"}}}}]}}
                ]
                for chunk in chunks:
                    self.wfile.write(f"data: {{json.dumps(chunk)}}\\n\\n".encode())
                self.wfile.write(b"data: [DONE]\\n\\n")
            else:
                resp = {{
                    "choices": [{{
                        "message": {{
                            "role": "assistant",
                            "content": "Mock success response!"
                        }}
                    }}],
                    "usage": {{
                        "prompt_tokens": 5,
                        "completion_tokens": 5
                    }}
                }}
                payload = json.dumps(resp).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{{"status": "ok"}}')

try:
    server = ThreadingHTTPServer(('127.0.0.1', port), MockLlamaServer)
    server.serve_forever()
except Exception as e:
    print("Mock server exception:", e)
"""


@unittest.skipIf(
    sys.platform.startswith("win"), "Mock executable not supported on Windows"
)
class TestGpuHangRecovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Create mock bin directory
        os.makedirs(MOCK_BIN_DIR, exist_ok=True)

        # Write mock llama-server script
        mock_script_path = os.path.join(MOCK_BIN_DIR, "llama-server")
        with open(mock_script_path, "w") as f:
            f.write(MOCK_LLAMA_SERVER.format(state_file_repr=repr(STATE_FILE)))

        # Make script executable
        os.chmod(mock_script_path, 0o755)

    @classmethod
    def tearDownClass(cls):
        # Clean up mock bin directory
        if os.path.exists(MOCK_BIN_DIR):
            shutil.rmtree(MOCK_BIN_DIR)

    def setUp(self):
        # Clear state file
        if os.path.exists(STATE_FILE):
            os.remove(STATE_FILE)

        # Resolve build directory and lemond binary
        cli_binary = get_cli_binary()
        if cli_binary:
            build_dir = os.path.dirname(cli_binary)
            name = "lemond.exe" if os.name == "nt" else "lemond"
            lemond_bin = os.path.join(build_dir, name)
            if not os.path.exists(lemond_bin):
                lemond_bin = get_default_lemond_binary()
                build_dir = os.path.dirname(lemond_bin)
        else:
            lemond_bin = get_default_lemond_binary()
            build_dir = os.path.dirname(lemond_bin)

        cache_dir = os.path.join(build_dir, "test_cache")
        os.makedirs(cache_dir, exist_ok=True)

        # Backup config.json if it exists
        self.config_backup = None
        self.config_path = os.path.join(cache_dir, "config.json")
        if os.path.exists(self.config_path):
            try:
                with open(self.config_path, "r") as f:
                    self.config_backup = f.read()
            except Exception:
                pass

        # Write config.json to override the llama-server binaries with our mock path
        mock_exe = os.path.join(MOCK_BIN_DIR, "llama-server")
        config_data = {
            "config_version": 2,
            "llamacpp": {
                "cpu_bin": mock_exe,
                "vulkan_bin": mock_exe,
                "cuda_bin": mock_exe,
                "rocm_bin": mock_exe,
            },
        }
        try:
            with open(self.config_path, "w") as f:
                json.dump(config_data, f)
        except Exception as e:
            print("Failed to write mock config:", e)

        # Start lemond in a background process with PATH overridden and fast watchdog
        env = os.environ.copy()
        env["PATH"] = f"{MOCK_BIN_DIR}{os.pathsep}{env.get('PATH', '')}"
        env["LEMONADE_BACKEND_WATCHDOG_POLL_SECONDS"] = "1"

        print("Starting lemond...")
        self.lemond_proc = subprocess.Popen(
            [lemond_bin, "--port", str(PORT), cache_dir],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        # Wait for lemond to be healthy
        healthy = False
        for _ in range(30):
            try:
                resp = requests.get(f"{BASE_URL}/api/v1/health", timeout=1)
                if resp.status_code == 200:
                    healthy = True
                    break
            except requests.RequestException:
                pass
            time.sleep(0.5)

        if not healthy:
            self.lemond_proc.terminate()
            try:
                self.lemond_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.lemond_proc.kill()
            stdout, stderr = self.lemond_proc.communicate()
            print("Lemond stdout:", stdout)
            print("Lemond stderr:", stderr)
            self.fail("lemond failed to start / respond to health check")

    def tearDown(self):
        # Terminate lemond
        if self.lemond_proc:
            self.lemond_proc.terminate()
            try:
                self.lemond_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.lemond_proc.kill()
                self.lemond_proc.wait()

        # Restore config.json backup
        if hasattr(self, "config_path"):
            try:
                if self.config_backup is not None:
                    with open(self.config_path, "w") as f:
                        f.write(self.config_backup)
                elif os.path.exists(self.config_path):
                    os.remove(self.config_path)
            except Exception as e:
                print("Failed to restore config backup:", e)

    def _reset_attempt_count(self):
        if os.path.exists(STATE_FILE):
            os.remove(STATE_FILE)

    def _get_attempt_count(self):
        if os.path.exists(STATE_FILE):
            try:
                with open(STATE_FILE, "r") as f:
                    state = json.load(f)
                    return state.get("attempt", 0)
            except Exception:
                pass
        return 0

    def _get_attempt_1_requests(self):
        if os.path.exists(STATE_FILE):
            try:
                with open(STATE_FILE, "r") as f:
                    state = json.load(f)
                    return state.get("attempt_1_requests", 0)
            except Exception:
                pass
        return 0

    def test_non_streaming_gpu_hang_recovery(self):
        """Test that a non-streaming GPU Hang/Compute Error triggers a reload and transparent retry."""
        self._reset_attempt_count()

        # Load the capability test model
        print("Loading test model...")
        load_resp = requests.post(
            f"{BASE_URL}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=30,
        )
        self.assertEqual(load_resp.status_code, 200)

        # Send chat completion request
        print("Sending chat completion request...")
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Hello!"}],
            "stream": False,
        }

        resp = requests.post(
            f"{BASE_URL}/api/v1/chat/completions", json=payload, timeout=30
        )

        # Verify request succeeded
        self.assertEqual(
            resp.status_code,
            200,
            f"Expected 200 OK, got {resp.status_code}: {resp.text}",
        )
        data = resp.json()
        self.assertIn("choices", data)
        self.assertEqual(
            data["choices"][0]["message"]["content"], "Mock success response!"
        )

        # Verify the mock server was started twice (attempt == 2)
        attempts = self._get_attempt_count()
        self.assertEqual(
            attempts,
            2,
            f"Expected 2 backend subprocess starts (retry), but got {attempts}",
        )
        print("[PASS] Non-streaming GPU Hang recovery succeeded!")

    def test_streaming_gpu_hang_recovery(self):
        """Test that a streaming GPU Hang/Compute Error triggers a reload and transparent retry."""
        self._reset_attempt_count()

        # Load the capability test model
        print("Loading test model...")
        load_resp = requests.post(
            f"{BASE_URL}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=30,
        )
        self.assertEqual(load_resp.status_code, 200)

        # Send streaming chat completion request
        print("Sending streaming chat completion request...")
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "Hello!"}],
            "stream": True,
        }

        resp = requests.post(
            f"{BASE_URL}/api/v1/chat/completions", json=payload, stream=True, timeout=30
        )

        # Consume the stream
        self.assertEqual(
            resp.status_code,
            200,
            f"Expected 200 OK, got {resp.status_code}: {resp.text}",
        )

        tokens = []
        for line in resp.iter_lines():
            if line:
                line_str = line.decode("utf-8")
                if line_str.startswith("data: ") and not line_str.endswith("[DONE]"):
                    try:
                        chunk = json.loads(line_str[6:])
                        content = chunk["choices"][0]["delta"].get("content", "")
                        if content:
                            tokens.append(content)
                    except Exception:
                        pass

        full_response = "".join(tokens)
        self.assertEqual(full_response, "Mock stream success!")

        # Verify the mock server was started twice (attempt == 2)
        attempts = self._get_attempt_count()
        self.assertEqual(
            attempts,
            2,
            f"Expected 2 backend subprocess starts (retry), but got {attempts}",
        )
        print("[PASS] Streaming GPU Hang recovery succeeded!")

    def test_concurrent_gpu_hang_recovery(self):
        """Test that concurrent GPU Hang/Compute Errors trigger exactly one reload and succeed."""
        self._reset_attempt_count()

        # Load the capability test model
        print("Loading test model...")
        load_resp = requests.post(
            f"{BASE_URL}/api/v1/load",
            json={"model_name": ENDPOINT_TEST_MODEL},
            timeout=30,
        )
        self.assertEqual(load_resp.status_code, 200)

        # Send concurrent chat completion requests
        print("Sending concurrent chat completion requests...")
        payload = {
            "model": ENDPOINT_TEST_MODEL,
            "messages": [{"role": "user", "content": "concurrent-test"}],
            "stream": False,
        }

        def send_request():
            try:
                return requests.post(
                    f"{BASE_URL}/api/v1/chat/completions", json=payload, timeout=30
                )
            except Exception as e:
                return e

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(send_request) for _ in range(2)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        for i, resp in enumerate(results):
            if isinstance(resp, Exception):
                self.fail(f"Request {i} raised exception: {resp}")
            self.assertEqual(
                resp.status_code,
                200,
                f"Request {i} expected 200 OK, got {resp.status_code}: {resp.text}",
            )
            data = resp.json()
            self.assertIn("choices", data)
            self.assertEqual(
                data["choices"][0]["message"]["content"], "Mock success response!"
            )

        # Verify that only ONE reload actually happened.
        # So attempt count should be 2 (first start + one reload).
        attempts = self._get_attempt_count()
        self.assertEqual(
            attempts,
            2,
            f"Expected exactly 2 starts (1 initial + 1 reload), but got {attempts}",
        )

        # Verify that both concurrent requests were handled by attempt 1
        attempt_1_reqs = self._get_attempt_1_requests()
        self.assertEqual(
            attempt_1_reqs,
            2,
            f"Expected exactly 2 requests to hit attempt 1, but got {attempt_1_reqs}",
        )
        print("[PASS] Concurrent GPU Hang recovery succeeded!")


if __name__ == "__main__":
    run_server_tests(TestGpuHangRecovery, "GPU HANG / COMPUTE ERROR RECOVERY TESTS")
