"""Self-tests using an in-process HTTP fixture instead of lemond."""

import json
from types import SimpleNamespace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock

from harness import Client, IsolatedLemond, Response, free_port, multipart
import server_contract as contracts


class FixtureHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/missing":
            self.reply(404, {"error": "not found"})
        else:
            self.reply(200, {"value": "ok"})

    def do_POST(self):
        payload = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.reply(
            200,
            {
                "body": payload.decode(),
                "auth": self.headers.get("Authorization"),
                "content_type": self.headers.get("Content-Type"),
            },
        )

    def reply(self, code, value):
        body = json.dumps(value).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


class HarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.client = Client(cls.server.server_address[1], "fixture-token")

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=5)

    def test_json_get(self):
        reply = self.client.get("/value")
        self.assertEqual((reply.status, reply.json), (200, {"value": "ok"}))

    def test_status_preserved(self):
        self.assertEqual(self.client.get("/missing").status, 404)

    def test_json_post_round_trip(self):
        reply = self.client.post("/send", data={"nested": [1, {"yes": True}]})
        self.assertEqual(json.loads(reply.json["body"]), {"nested": [1, {"yes": True}]})

    def test_token_added(self):
        reply = self.client.get("/value")
        self.assertEqual(reply.status, 200)
        reply = self.client.post("/test", data={})
        self.assertEqual(reply.json["auth"], "Bearer fixture-token")

    def test_explicit_auth_header_overrides_client_token(self):
        reply = self.client.post(
            "/test", data={}, headers={"Authorization": "Bearer overridden"}
        )
        self.assertEqual(reply.json["auth"], "Bearer overridden")

    def test_raw_body_not_coerced_to_json(self):
        reply = self.client.post(
            "/test", raw=b"{wrong", headers={"Content-Type": "application/json"}
        )
        self.assertEqual(reply.json["body"], "{wrong")

    def test_missing_leading_slash_is_rejected(self):
        with self.assertRaises(ValueError):
            self.client.get("https://external.invalid/")

    def test_conflicting_body_inputs_rejected(self):
        with self.assertRaises(ValueError):
            self.client.request("POST", "/", data={}, raw=b"x")

    def test_multipart_has_closing_boundary(self):
        content, headers = multipart(
            {"model": "hi", "temperature": "0.1"}, {"file": ("speech.wav", b"RIFF")}
        )
        self.assertTrue(
            content.endswith(b"--lemonade-refactor-verified-boundary--\r\n")
        )
        self.assertIn(b'name="file"', content)
        self.assertIn(b"RIFF", content)
        self.assertIn("multipart/form-data", headers["Content-Type"])

    def test_port_is_ephemeral(self):
        self.assertGreater(free_port(), 0)

    def test_response_header_is_case_insensitive(self):
        reply = Response(200, {"content-type": "application/json"}, b"{}")
        self.assertEqual(reply.header("Content-Type"), "application/json")

    def test_empty_body_is_not_valid_json(self):
        with self.assertRaises(ValueError):
            Response(200, {}, b"").json

    def test_isolated_server_options_are_written_without_running_binary(self):
        daemon = IsolatedLemond(
            "/not/an/executable", options={"disable_model_filtering": True}
        )
        self.addCleanup(daemon.close)
        health = Response(200, {}, b'{"status":"ok"}')
        config = Response(
            200, {}, json.dumps({"models_dir": str(daemon.models)}).encode("utf-8")
        )
        with mock.patch("harness.subprocess.Popen") as popen, mock.patch.object(
            daemon.api, "get", return_value=health
        ), mock.patch.object(daemon.anonymous, "get", return_value=config):
            popen.return_value.poll.return_value = None
            daemon.start()
            written = json.loads((daemon.config / "config.json").read_text())
            self.assertTrue(written["disable_model_filtering"])
            self.assertTrue(written["offline"])
            self.assertTrue(written["no_fetch_executables"])
            self.assertEqual(written["models_dir"], str(daemon.models))
            popen.assert_called_once()
            env = popen.call_args.kwargs["env"]
            self.assertEqual(env["HOME"], str(daemon.root))
            self.assertEqual(env["HF_HUB_CACHE"], str(daemon.models))
            self.assertEqual(env["LEMONADE_DISABLE_TELEMETRY"], "1")

    def test_default_isolated_server_does_not_override_recipe_filter(self):
        daemon = IsolatedLemond("/not/an/executable")
        self.addCleanup(daemon.close)
        self.assertNotIn("disable_model_filtering", daemon.options)

    def test_server_rejects_models_directory_outside_isolation(self):
        daemon = IsolatedLemond(
            "/not/an/executable", options={"models_dir": "/not/isolated/models"}
        )
        self.addCleanup(daemon.close)
        with self.assertRaisesRegex(ValueError, "isolated models directory"):
            daemon.start()

    def test_gui3_bridge_is_skipped_only_when_disabled(self):
        case = contracts.ManifestContract("test_gui3_real_api_client_over_http")
        case.daemon = SimpleNamespace(port=12345)
        self.assertFalse(
            getattr(
                case.test_gui3_real_api_client_over_http, "__unittest_skip__", False
            ),
            "A skip decorator evaluated at import time cannot observe --gui3",
        )
        with mock.patch.object(contracts, "WITH_GUI3", False):
            with self.assertRaises(unittest.SkipTest):
                case.test_gui3_real_api_client_over_http()

    def test_gui3_bridge_dispatches_when_enabled(self):
        case = contracts.ManifestContract("test_gui3_real_api_client_over_http")
        case.daemon = SimpleNamespace(port=12345)
        outcome = SimpleNamespace(
            returncode=0, stdout="GUI3 real API checks passed: 20\n", stderr=""
        )
        with mock.patch.object(contracts, "WITH_GUI3", True), mock.patch.object(
            contracts.subprocess, "run", return_value=outcome
        ) as run:
            case.test_gui3_real_api_client_over_http()
        run.assert_called_once()
        args, kwargs = run.call_args
        self.assertEqual(args[0][0], "node")
        self.assertTrue(args[0][1].endswith("gui3_bridge.cjs"))
        self.assertEqual(kwargs["env"]["LEMONADE_HTTP_REFACTOR_ISOLATED"], "1")
        self.assertEqual(
            kwargs["env"]["LEMONADE_HTTP_REFACTOR_URL"], "http://127.0.0.1:12345"
        )

    def test_temporary_working_directory_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertTrue(Path(tmp).is_dir())


if __name__ == "__main__":
    unittest.main()
