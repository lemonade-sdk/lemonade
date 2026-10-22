"""Pure local tests for the HTTP harness; these never start Lemonade."""

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import struct
import tempfile
import threading
import unittest

from server_contract import Api, encoded, free_port, write_gguf


class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/missing":
            body = b'{"error":"not found"}'
            self.send_response(404)
        else:
            body = b'{"value":true}'
            self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        result = json.dumps({"received": json.loads(payload)}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(result)))
        self.end_headers()
        self.wfile.write(result)

    def log_message(self, *_):
        pass


class HttpHarnessSelfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.client = Api(cls.server.server_address[1])

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=3)

    def test_get_json(self):
        self.assertEqual(self.client.get("/value"), (200, {"value": True}))

    def test_error_is_returned_without_swallowing_code(self):
        self.assertEqual(self.client.get("/missing"), (404, {"error": "not found"}))

    def test_post_preserves_nested_payload(self):
        data = {"model_name": "user.example", "checkpoints": {"main": "org/repo:file"}}
        self.assertEqual(self.client.post("/register", data), (200, {"received": data}))

    def test_url_encoding_handles_slashes_and_spaces(self):
        self.assertEqual(encoded("model/a b"), "model%2Fa%20b")

    def test_gguf_fixture_header_is_valid(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "a" / "model.gguf"
            write_gguf(path)
            data = path.read_bytes()
            self.assertEqual(data[:4], b"GGUF")
            self.assertEqual(struct.unpack("<IQQ", data[4:]), (3, 0, 0))

    def test_allocated_port_is_ephemeral(self):
        self.assertTrue(0 < free_port() < 65536)

    def test_plain_text_and_empty_response_parser(self):
        self.assertEqual(Api._parse(b"hello"), "hello")
        self.assertIsNone(Api._parse(b""))


if __name__ == "__main__":
    unittest.main()
