"""
One-shot / piped `lemonade chat` tests.

Runs the lemonade CLI against a local mock OpenAI-style server, so no real
server or model is needed.

Usage:
    python server_chat_oneshot.py
    python server_chat_oneshot.py --cli-binary /path/to/lemonade
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from utils.test_models import get_default_cli_binary

_config = {"cli_binary": None}

MODEL = "Mock-Model"
LONG_STREAM_PIECES = 300


class MockState:
    loaded_model = MODEL
    fail_stream = False
    fault = None
    last_chat_request = None
    pieces_sent = 0
    stream_finished = threading.Event()


class MockHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send_json(self, obj, status=200):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/api/v1/health":
            loaded = MockState.loaded_model
            self._send_json(
                {
                    "model_loaded": loaded,
                    "all_models_loaded": [{"model_name": loaded}] if loaded else [],
                }
            )
        elif self.path.startswith("/api/v1/models/"):
            self._send_json({"id": self.path.rsplit("/", 1)[-1], "downloaded": True})
        else:
            self._send_json({"error": {"message": "not found"}}, 404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/api/v1/load":
            self._send_json({"status": "success"})
        elif self.path == "/api/v1/chat/completions":
            req = json.loads(body)
            MockState.last_chat_request = req
            if req.get("stream"):
                self._stream(req)
            else:
                choice = {"finish_reason": "stop"}
                if MockState.fault == "empty_message":
                    choice["message"] = {}
                elif MockState.fault == "tool_call":
                    choice["message"] = {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_1",
                                "type": "function",
                                "function": {"name": "f", "arguments": "{}"},
                            }
                        ],
                    }
                elif MockState.fault != "no_message":
                    choice["message"] = {"role": "assistant", "content": "Hello there"}
                self._send_json(
                    {
                        "model": req["model"],
                        "choices": [choice],
                        "usage": {"prompt_tokens": 3, "completion_tokens": 2},
                    }
                )
        else:
            self._send_json({"error": {"message": "not found"}}, 404)

    def _stream(self, req):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def event(obj):
            self.wfile.write(f"data: {json.dumps(obj)}\n\n".encode())
            self.wfile.flush()

        if MockState.fault == "long_stream":
            try:
                for _ in range(LONG_STREAM_PIECES):
                    event({"choices": [{"delta": {"content": "word "}}]})
                    MockState.pieces_sent += 1
                    time.sleep(0.02)
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
            except OSError:
                pass
            finally:
                MockState.stream_finished.set()
            return

        for piece in ("Hello", " there"):
            event({"choices": [{"delta": {"content": piece}}]})
        if MockState.fault == "bad_stream_event":
            self.wfile.write(b"data: {not json\n\n")
        if MockState.fail_stream:
            return
        event({"choices": [], "usage": {"prompt_tokens": 3, "completion_tokens": 2}})
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


class ChatOneShotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        MockState.loaded_model = MODEL
        MockState.fail_stream = False
        MockState.fault = None
        MockState.last_chat_request = None
        MockState.pieces_sent = 0
        MockState.stream_finished.clear()

    def chat(self, *args, stdin="", stdout=None):
        cmd = [
            _config["cli_binary"] or get_default_cli_binary(),
            "--no-discovery",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.port),
            "chat",
            *args,
        ]
        return subprocess.run(
            cmd,
            input=stdin,
            stdout=stdout if stdout is not None else subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            timeout=60,
        )

    def sent_user_content(self):
        return MockState.last_chat_request["messages"][-1]["content"]

    def test_prompt_streams_plain_text(self):
        r = self.chat(MODEL, "-p", "hi")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "Hello there\n")
        self.assertEqual(r.stderr, "")
        self.assertEqual(self.sent_user_content(), "hi")

    def test_load_progress_goes_to_stderr(self):
        MockState.loaded_model = "Other-Model"
        r = self.chat(MODEL, "-p", "hi")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "Hello there\n")
        self.assertIn("Loading model", r.stderr)

    def test_no_stream(self):
        r = self.chat(MODEL, "-p", "hi", "--no-stream")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "Hello there\n")
        self.assertFalse(MockState.last_chat_request["stream"])

    def test_stdin_only(self):
        r = self.chat(MODEL, stdin="log line\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.sent_user_content(), "log line\n")

    def test_non_ascii_prompt_reaches_server(self):
        prompt = "café ☕ 日本語"
        r = self.chat(MODEL, "-p", prompt)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.sent_user_content(), prompt)

    def test_prompt_precedes_stdin(self):
        r = self.chat(MODEL, "-p", "Summarize", stdin="log line\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.sent_user_content(), "Summarize\n\nlog line\n")

    def test_system_prompt_sent(self):
        r = self.chat(MODEL, "-p", "hi", "--system", "be brief")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(
            MockState.last_chat_request["messages"][0],
            {"role": "system", "content": "be brief"},
        )

    def test_uses_loaded_model_when_omitted(self):
        r = self.chat("-p", "hi")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(MockState.last_chat_request["model"], MODEL)

    def test_json_output(self):
        r = self.chat(MODEL, "-p", "hi", "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = json.loads(r.stdout)
        self.assertEqual(out["model"], MODEL)
        self.assertEqual(out["message"]["content"], "Hello there")
        self.assertEqual(out["finish_reason"], "stop")
        self.assertEqual(out["usage"]["completion_tokens"], 2)
        self.assertNotIn("session_id", out)

    def test_empty_input_is_usage_error(self):
        for args, stdin in ((("-p", "  "), ""), ((), "  \n")):
            r = self.chat(MODEL, *args, stdin=stdin)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertEqual(r.stdout, "")

    def test_no_model_is_usage_error(self):
        MockState.loaded_model = None
        r = self.chat("-p", "hi")
        self.assertEqual(r.returncode, 2)
        self.assertEqual(r.stdout, "")

    def test_json_without_one_shot_is_usage_error(self):
        # Closed-TTY stdin can't be simulated portably; empty piped stdin is still rejected.
        r = self.chat(MODEL, "--json", stdin="")
        self.assertEqual(r.returncode, 2)

    def test_unknown_option_is_usage_error(self):
        r = self.chat("--bogus")
        self.assertEqual(r.returncode, 2)

    def test_stream_failure_keeps_partial_output_and_fails(self):
        MockState.fail_stream = True
        r = self.chat(MODEL, "-p", "hi")
        self.assertEqual(r.returncode, 1)
        self.assertTrue(r.stdout.startswith("Hello there"))
        self.assertIn("Error", r.stderr)

    def test_response_without_message_fails(self):
        MockState.fault = "no_message"
        for extra in ((), ("--json",)):
            r = self.chat(MODEL, "-p", "hi", "--no-stream", *extra)
            self.assertEqual(r.returncode, 1, r.stderr)
            self.assertEqual(r.stdout, "")
            self.assertIn("Error", r.stderr)

    def test_malformed_stream_event_fails(self):
        MockState.fault = "bad_stream_event"
        r = self.chat(MODEL, "-p", "hi")
        self.assertEqual(r.returncode, 1)
        self.assertTrue(r.stdout.startswith("Hello there"))
        self.assertIn("malformed", r.stderr)

    def test_empty_message_object_fails(self):
        MockState.fault = "empty_message"
        for extra in ((), ("--json",)):
            r = self.chat(MODEL, "-p", "hi", "--no-stream", *extra)
            self.assertEqual(r.returncode, 1, r.stderr)
            self.assertEqual(r.stdout, "")
            self.assertIn("Error", r.stderr)

    def test_tool_call_message_with_null_content_succeeds(self):
        MockState.fault = "tool_call"
        r = self.chat(MODEL, "-p", "hi", "--no-stream", "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        message = json.loads(r.stdout)["message"]
        self.assertIsNone(message["content"])
        self.assertEqual(message["tool_calls"][0]["id"], "call_1")

    @unittest.skipUnless(os.path.exists("/dev/full"), "needs /dev/full")
    def test_stdout_write_failure_aborts_stream(self):
        MockState.fault = "long_stream"
        with open("/dev/full", "wb") as full:
            r = self.chat(MODEL, "-p", "hi", stdout=full)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("failed to write to stdout", r.stderr)
        MockState.stream_finished.wait(5)
        self.assertLess(MockState.pieces_sent, LONG_STREAM_PIECES)

    def test_unreachable_server_fails(self):
        cmd = [
            _config["cli_binary"] or get_default_cli_binary(),
            "--no-discovery",
            "--port",
            "1",
            "chat",
            MODEL,
            "-p",
            "hi",
        ]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli-binary", default=None)
    args, remaining = parser.parse_known_args()
    _config["cli_binary"] = args.cli_binary
    unittest.main(argv=[sys.argv[0]] + remaining)
